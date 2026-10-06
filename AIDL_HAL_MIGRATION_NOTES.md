# HdmiCec AIDL HAL Migration — Design Notes

This file holds the design detail, rationale and history that the Doxygen comments in the
AIDL HAL back-end's net-new code, its test doubles and its tests no longer carry; those code
comments are deliberately brief. Each per-file section below (one per source file, or per part
of a large file) expands that file's comments with one `###` entry per symbol, and two further
sections describe the DeviceType-derived logical-address registration and the fixed physical
address 1.0.0.0 on the AIDL back-end, with any earlier statement those changes made untrue
kept only where it is marked as superseded.

**Contents**

- [ccec/src/Driver.cpp](#ccecsrcdrivercpp)
- ccec/src/DriverAidlImpl.hpp
  - [part 1 of 2](#ccecsrcdriveraidlimplhpp-part-1-of-2)
  - [part 2 of 2](#ccecsrcdriveraidlimplhpp-part-2-of-2)
- ccec/src/DriverAidlImpl.cpp
  - [part 1 of 2](#ccecsrcdriveraidlimplcpp-part-1-of-2)
  - [part 2 of 2](#ccecsrcdriveraidlimplcpp-part-2-of-2)
- [Logical-address allocation and registration (AIDL back-end)](#logical-address-allocation-and-registration-aidl-back-end)
- [Physical address (AIDL back-end)](#physical-address-aidl-back-end)
- [mocks/hdmicec/fake_hdmi_cec_aidl_service.h](#mockshdmicecfake_hdmi_cec_aidl_serviceh)
- [mocks/hdmicec/fake_hdmi_cec_aidl_service.cpp](#mockshdmicecfake_hdmi_cec_aidl_servicecpp)
- [mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp](#mockshdmicecfake_hdmi_cec_aidl_service_hostcpp)
- [tests/L1Tests/test_main.cpp](#testsl1teststest_maincpp)
- tests/L1Tests/ccec/test_DriverAidl.cpp
  - [part 1 of 6](#testsl1testsccectest_driveraidlcpp-part-1-of-6)
  - [part 2 of 6](#testsl1testsccectest_driveraidlcpp-part-2-of-6)
  - [part 3 of 6](#testsl1testsccectest_driveraidlcpp-part-3-of-6)
  - [part 4 of 6](#testsl1testsccectest_driveraidlcpp-part-4-of-6)
  - [part 5 of 6](#testsl1testsccectest_driveraidlcpp-part-5-of-6)
  - [part 6 of 6](#testsl1testsccectest_driveraidlcpp-part-6-of-6)
- tests/L2Tests/test_main.cpp
  - [part 1 of 2](#testsl2teststest_maincpp-part-1-of-2)
  - [part 2 of 2](#testsl2teststest_maincpp-part-2-of-2)
- tests/L2Tests/ccec/test_DualPathIntegration.cpp
  - [part 1 of 2](#testsl2testsccectest_dualpathintegrationcpp-part-1-of-2)
  - [part 2 of 2](#testsl2testsccectest_dualpathintegrationcpp-part-2-of-2)

## ccec/src/Driver.cpp

### SELECTED_BACK_END_LOG_FORMAT

- One format string with one substitution, the back-end name, so the emitted line is identical
  across every selection arm apart from that name.
- It is a test and tooling contract rather than a diagnostic convenience, so it is defined once,
  in `Driver.cpp`, and referenced nowhere else.
- Consumers: the functional suites match it to establish which back-end resolved; the coverage
  runner greps it once per invocation and treats its absence as a failed invocation; device-level
  validation relies on it wherever the concrete back-end headers are out of scope.
- It is the only way the selection is observable. No introspection API is added to the `Driver`
  interface, deliberately, because that would grow the middleware public surface.

### resolveBackEnd

- It is the single selection point of the middleware. `DriverImpl` (legacy in-process C ABI) and
  `DriverAidlImpl` (out-of-process `com.rdk.hal.hdmicec` AIDL HAL) implement the same `Driver`
  interface and are both compiled into the library in every build, for every SOC vendor.
- Selection is decided at run time on the AIDL service's availability **and** compatibility. A
  service that is present but whose metadata halcompat rejects selects the legacy back-end exactly
  as an absent one does, which is why the question asked is `isServiceAvailable()`, not a bare
  presence test.
- Construct-then-query order is load bearing. Gating construction on availability would be unsafe:
  the availability query is the first thing in the process that may touch libbinder, and on the
  pinned binder stack an unguarded lookup on a platform with no binder driver aborts rather than
  returning an error. `DriverAidlImpl`'s constructor touches no binder, so constructing it is safe
  on a legacy-only SOC. `isServiceAvailable()` never propagates an exception, and each of its
  probes is bounded and declines rather than aborts. Its guarantee is not unconditional: the
  lookup and metadata transactions after the last probe have no client-side deadline (see
  **Residual acquisition window** under `CCEC::DriverAidlImpl::isServiceAvailable()`).
- Three arms exist, each logged, because "absent" and "present but not usable" are different
  platform conditions that validation gates separately:
  1. the service is present and compatible, so the AIDL back-end is selected;
  2. the binder transport is reachable but no compatible service resolved (none registered, or the
     registered one cannot be spoken to compatibly), so the legacy back-end is selected;
  3. the binder transport itself is unavailable, so the legacy back-end is selected.
- Arms 2 and 3 are told apart through `DriverAidlImpl::unavailabilityReason()`. The query runs the
  bounded preflight as its own first stage and records which stage declined, so the record is
  reported rather than re-derived. Re-deriving it would pay the preflight's context-manager timeout
  a second time, and a servicemanager appearing or dying between the two calls could make the
  reported reason name a condition that did not cause the fallback.
- A compatibility rejection is logged by `DriverAidlImpl::isServiceAvailable()` itself as a
  verdict: halcompat rejected the service for one of three rules (an empty or `"-1"` interface
  hash, an unfrozen development server, or an interface version outside the compiled-against era
  and major), and which one applied is not reported, because the metadata the decision read
  cannot be recovered. The server's hash and version follow as observations made after the
  decision, never as its cause, with a note when they would themselves have been accepted; if
  that read-back fails, only the rejection is logged. `resolveBackEnd` cannot observe the cause
  and must not invent it.
- Both candidates have static storage duration, so the returned reference stays valid for the
  lifetime of the process.
- It is called only from the one-time initializer of `Driver::getInstance()`, which the language
  serializes, so no lock is taken. `Driver::instanceMutex` in particular is not used: it is
  declared but never defined anywhere in the tree, and referencing it would leave the library with
  an undefined symbol.
- Neither construction nor the query throws, so the enclosing static cannot be left uninitialized
  and re-entered on a later call.
- Re-entering `Driver::getInstance()` from either back-end's constructor or availability query
  would re-enter the initializer that is still running, which is undefined behaviour rather than a
  recoverable error. Both back-ends reach the incoming frame queue through their own accessor
  precisely so this cannot happen.

### resolveBackEnd — fallback reason (function body)

- Calling `DriverAidlImpl::isBinderPreflightOk()` in the body to work out which arm declined would
  pay the context-manager timeout a second time, doubling the worst case added to `LibCCEC::init()`
  on the platform least able to absorb it, and the second answer could differ from the first.
- The reason phrases live beside their producer in `DriverAidlImpl.cpp`; two of the three are a
  contract, because the coverage runner and the L2 tier both transcribe the emitted line, so
  rewording either breaks them.
- The "not usable" line is worded deliberately unlike the selected-path line, so grepping for the
  selected-path literal yields exactly one hit per process.
- A `NULL` reason is not expected (`isServiceAvailable()` records a reason on every false exit) but
  is handled rather than assumed, logged as a warning, because a silent fallback with no reason
  would be undiagnosable.
- Declaring the legacy back-end first makes it the last destroyed, matching its role as the
  fallback; nothing gates whether the AIDL back-end exists.

## ccec/src/DriverAidlImpl.hpp (part 1 of 2)

- **Superseded statements (recorded, not current behaviour).** The pre-refine class comment held that exactly three observable differences from the legacy back-end were authorized — write()'s 16-byte limit, writeAsync()'s `OperationNotSupportedException` and addLogicalAddress()'s coarser failure category — and that any further difference was a defect. The AIDL back-end now also registers one logical address, derived from the device's DeviceType, through `IHdmiCecController::addLogicalAddresses()` once open() reaches OPENED, and getPhysicalAddress() returns the fixed physical address 1.0.0.0 (`0x01000000`) without any HAL call. The same comment listed "getLogicalAddress() ignoring its `devType` argument" among the reproduced legacy behaviours; the address getLogicalAddress() reads back through `IHdmiCec::getLogicalAddresses()` is now the one registered from the DeviceType at enable, so that phrase no longer describes how the address is determined.

### `DriverAidlImpl.hpp` (file block)

- This is the second of the two `CCEC::Driver` implementations the middleware ships. It adapts the out-of-process `com.rdk.hal.hdmicec` AIDL HAL onto the same contract DriverImpl implements over the legacy in-process C ABI (`libRCECHal.so`).
- Exactly one back-end is selected, once, during initialization, by `Driver::getInstance()`, from a runtime query for the `"HdmiCec"` binder service; the selection is stable for the lifetime of the process.
- Nothing above the Driver seam changes: the frame queue, the Bus reader and writer threads, Connection, FrameListener and LibCCEC are untouched. Only the thread that produces received frames into the incoming queue differs between the back-ends.
- The header is absent from the `nobase_include_HEADERS` list in `hdmicec/Makefile.am`, exactly as `DriverImpl.hpp` is, which is what lets a second back-end exist without altering the middleware public API. Production code reaches it only from `ccec/src`; test translation units use `#include "../../../ccec/src/DriverAidlImpl.hpp"`.
- Include order is load bearing. The generated stubs open `namespace com::rdk::hal::hdmicec` and transitively pull the `binder/` and `utils/` SDK headers that open `namespace android`. Including any of them after `CCEC_BEGIN_NAMESPACE` would nest those declarations as `CCEC::com::rdk::hal::hdmicec` and `CCEC::android`, which breaks the link and silently violates the one-definition rule.
- C++17 is required because the generated stubs include `<optional>` and `IHdmiCec::getProperty()` takes a `std::optional<PropertyValue>*`; a C++14 translation unit cannot compile the header.
- This is the C++ libbinder AIDL backend, not the NDK backend: the pointer type is `android::sp<>`, the status type is `android::binder::Status`, and the server bases are the generated `Bn*` classes.

### `#include <atomic>` (superseded)

- **Superseded, recorded only as such.** The header formerly included `<atomic>` because the lifecycle `status` member was an `std::atomic<int>`. `status` is now a plain `int`, as `DriverImpl::status` is, and nothing in the header needs `<atomic>`, so the include is gone. See `DriverAidlImpl::status`.

### AIDL and binder includes

- `IHdmiCec.h` and `IHdmiCecController.h` are required at header level rather than forward declared, because the `android::sp<>` session members are complete-type class members. They must stay above `CCEC_BEGIN_NAMESPACE` (see the file block).
- `<com/rdk/hal/hdmicec/BnHdmiCecEventListener.h>` is deliberately not included. The nested EventListener is forward declared in the header and defined in `DriverAidlImpl.cpp`, which keeps the listener's server-side base out of every translation unit that merely includes the header.

### `LOG_FATAL` (`#undef` and restoration)

- Two independent definitions collide. The binder SDK's `log/log_main.h` defines `LOG_FATAL` as a function-like macro, guarded by `#ifndef`; `ccec/Util.hpp` defines it, unguarded, as the integer log level 0, and CCEC code consumes it as a value (`CCEC_LOG(LOG_FATAL, ...)`).
- Left alone, the two include orders behave differently: binder-then-ccec merely warns about a redefinition, while ccec-then-binder leaves the function-like macro in force and turns any later use of `LOG_FATAL` as a value into a hard compile error.
- Dropping the binder spelling and restoring the CCEC value makes the header order independent, with no diagnostic. Nothing in the binder SDK headers consumes a bare `LOG_FATAL` — they use `LOG_FATAL_IF` and `LOG_ALWAYS_FATAL`, which are defined separately and left untouched — so removing it costs nothing.
- The `#ifndef LOG_FATAL` restoration covers a translation unit that included `ccec/Util.hpp` before this header, whose include guard then skips Util.hpp's definition. The value is identical to Util.hpp's, and an existing definition is never redefined.

### `CCEC::DriverAidlImpl`

- Every `CCEC::Driver` virtual keeps the legacy signature. Its guards, statement order and exceptions are the legacy ones, with the `com.rdk.hal.hdmicec` AIDL calls substituted for the legacy HDMI CEC C API, except for the per-method departures listed under [DriverAidlImpl.cpp (file header)](#driveraidlimplcpp-file-header).
- Legacy behaviour that reads like a defect is reproduced rather than improved, because callers and the existing test suite depend on it: the `#if 0`'d throws in open() and close(), removeLogicalAddress() discarding the HAL return, close() leaving the local address list populated, and writeAsync() doing frame work before its state guard.
- Observable differences from the legacy back-end, each documented on its method, the logical-address ones in full under [Logical-address allocation and registration (AIDL back-end)](#logical-address-allocation-and-registration-aidl-back-end):
  - write()'s 16-byte frame limit;
  - writeAsync()'s `OperationNotSupportedException`;
  - addLogicalAddress()'s coarser failure category;
  - addLogicalAddress()'s one-address replacement:
    - an address above `0xE` raises `AddressNotAvailableException` before anything is released;
    - a different address replaces the held one only after its release is confirmed, by an ok `true` from `removeLogicalAddresses()` or by an `IHdmiCec::getLogicalAddresses()` read-back that succeeds without it;
    - an unconfirmed release keeps the held address recorded, adds nothing and raises `AddressNotAvailableException` when the HAL declined the release and still lists the address, else `IOException`;
    - after a confirmed release, a failed add records nothing locally; a declined add leaves nothing pending, while an add that fails in transport or raises keeps its address in `unconfirmedReleaseAddress`, which the next add settles (releases, or confirms absent) before adding;
  - removeLogicalAddress()'s record of an unconfirmed release: it keeps the legacy shape, but the held address is kept in `unconfirmedReleaseAddress` from before the local removal until the HAL confirms its release, so a release that is declined, fails or raises is settled by the next addLogicalAddress() before it adds;
  - the DeviceType-derived logical address open() registers through `addLogicalAddresses()`, which getLogicalAddress() reads back through `IHdmiCec::getLogicalAddresses()`;
  - and getPhysicalAddress()'s fixed 1.0.0.0.
- One defensive guard is delivered and registered for the specification owner rather than silently adopted: getLogicalAddress() reports no address when the HAL names one outside the AIDL contract range `0x0..0xE`, where the legacy back-end returns whatever the HAL wrote. It is reachable only when the HAL violates its own contract, and the alternative carries an out-of-contract logical address into the frame headers the middleware builds from it.
- write() handles a `SendMessageStatus` value outside the three documented enumerators as the legacy back-end handles an unrecognised status: it logs the value by number and returns normally. *Superseded:* an earlier revision raised `IOException` for such a value as a second defensive guard; review removed it because it was an observable difference outside the authorized list.
- read() is a byte-for-byte copy of the legacy body apart from the class name, flush loop included (see below).
- The incoming queue is not a difference: like the legacy queue it holds 32 entries, shared by received frames and close()'s NULL sentinel, so a close against a full queue drops its sentinel on both back-ends (see `INCOMING_QUEUE_CAPACITY`).
- The AIDL surface is split across two interfaces and nine of its thirteen methods are consumed. `IHdmiCec` supplies `open()`, `close()` and `getLogicalAddresses()`; `IHdmiCecController`, obtained from `open()`, supplies `addLogicalAddresses()`, `removeLogicalAddresses()` and `sendMessage()`; the listener supplies `onMessageReceived()`, `onStateChanged()` and `onMessageSent()`. `getState()` is not consumed because the middleware keeps its own state machine, `getProperty()` because the HAL properties have no legacy counterpart, and `registerEventListener()`/`unregisterEventListener()` because this back-end is the controlling client and receives events through the listener it hands to `open()`.
- Construction touching no binder is what makes the factory's construct-then-query shape safe on a legacy-only SOC.
- The receive path writes the incoming queue from a binder threadpool thread this class does not own. That queue is the existing cross-thread synchronization point and needs no change.

### `CCEC::DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS`

- isBinderPreflightOk() takes its timeout as an `unsigned int`, so a caller — a test, or a future platform-integration knob — can name a value far larger than any plausible `servicemanager` start-up delay, every millisecond of which `LibCCEC::init()` would spend blocked. The parameter is clamped to this ceiling, and the clamp is logged so a mis-set value is visible rather than silently obeyed.
- At 10000 ms against the 2000 ms default (the pre-refine comment called this "an order of magnitude above"), the default is never clamped and no legitimate integration value is truncated, while the probes' share of an initialization stall stays bounded by a figure a human can reason about.
- If the ceiling fell below `DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS`, the default itself would be clamped and the default path would log on every start-up.

### `CCEC::DriverAidlImpl::BINDER_NODE_MODE_TYPE_MASK`

- Written out for the same reason the protocol version crosses BinderPreflightProbe as a plain `unsigned int`: the header must compile where the binder kernel UAPI definitions are absent, so the file-type bits are extracted with a constant of the class's own rather than a macro from a header the file does not include.
- The value is fixed by POSIX and by the Linux ABI. The contract suite asserts it equals `S_IFMT` so the restatement cannot drift unnoticed; the same cross-check applies to every restated `BINDER_NODE_MODE_*` constant.

### `CCEC::DriverAidlImpl::BINDER_NODE_MODE_CHARACTER_DEVICE`

- The supported layouts are a devtmpfs node under `/dev`, or a binderfs node under `/dev/binderfs` reached directly or through a symlink. Anything else behind the configured path is not the binder driver, whatever it is named.

### `CCEC::DriverAidlImpl::BINDER_NODE_REQUIRED_OWNER_UID`

- The node is created by the kernel through devtmpfs, or by mounting binderfs, and is root-owned in both cases. A node at the configured path owned by anyone else is one an unprivileged process was able to create or replace, which is the precondition of the substitution the identity checks close. On the pinned stack, handing libbinder a substituted node is an abort, not an error return.

### `CCEC::DriverAidlImpl::BINDER_NODE_MODE_PERMISSION_MASK`

- `ACCESSPERMS` is `S_IRWXU | S_IRWXG | S_IRWXO`, restated for the same reason as `BINDER_NODE_MODE_TYPE_MASK`.
- The bits are extracted solely for a diagnostic. The documentation on isBinderPreflightOk() explains why node-permission restrictiveness is not something this middleware can require.

### `CCEC::DriverAidlImpl::INCOMING_QUEUE_CAPACITY`

- The capacity is named and passed to the queue explicitly by the constructor, so the capacity this class reasons about and the one the queue enforces are the same number by construction rather than by coincidence; a future change to the OSAL default cannot silently desynchronize them.
- `EventQueue::offer()` returns void and drops its argument silently when the queue is at capacity, so the receive path must establish for itself that there is room before it parts with ownership of a frame (offerReceivedFrame()).
- **The legacy queue contract, kept as it is.** DriverImpl leaves its queue on the OSAL default of 32, and received frames and close()'s NULL sentinel share those 32 slots. This queue is the same: received frames may fill all 32, and a close against a full queue has its sentinel dropped by `EventQueue::offer()`, exactly as on the legacy path. The receive path changes only which thread produces into the queue, so the capacity and the sentinel's fate under it stay the legacy ones.
- **Why the dropped sentinel does not strand the reader.** A reader blocked in `EventQueue::poll()` waits only on an empty queue, so it cannot be waiting when the sentinel meets a full one. It drains frames through ordinary returns of read(), and its next read() raises `InvalidStateException` at the entry guard because the state is no longer OPENED.
- Superseded: an earlier revision sized this queue at 33 and refused received frames at 32 so the sentinel always fit. That changed the full-queue close behaviour against the legacy queue and was withdrawn.

### `static_assert(INCOMING_QUEUE_CAPACITY == 32, ...)`

- The capacity is pinned to a literal, deliberately not to an expression: 32 is the capacity `EventQueue(size_t cap = 32)` in `osal/include/osal/EventQueue.hpp` gives DriverImpl's queue, which this queue must match entry for entry, sentinel included.
- Every test expectation over the queue derives from `INCOMING_QUEUE_CAPACITY` except one runtime assertion against the same literal, so a change to the constant fails to compile here before any test could pass against it.
- If the OSAL default changes, the assertion must be re-derived from the legacy queue rather than relaxed: the number to pin is whatever capacity DriverImpl's queue gets, not whatever this back-end happens to use.

### `CCEC::DriverAidlImpl::DriverAidlImpl()`

- No binder contact at construction is what the selection order requires: both back-ends are always constructed, and only then is the AIDL one asked whether its service came up. A constructor that reached for binder would abort the process on a legacy-only SOC before the fallback could be taken.
- Construction is binder-inert but not allocation-free: the incoming queue's `EventQueue` constructor allocates its deque (`osal/include/osal/EventQueue.hpp`), so ordinary allocation failure, raised as `std::bad_alloc`, is its one failure mode. Such an exception propagates out of `Driver::getInstance()`, whose function-local statics are then initialized again on the next call.

### `CCEC::DriverAidlImpl::~DriverAidlImpl()`

- A non-CLOSED instance is closed through this class's own close(), and an exception escaping that close is caught and logged.
- The legacy destructor has no listener to release. Here the listener is detached and released unconditionally after the close attempt: swallowing the close exception is required of a destructor, but a swallowed close failure is exactly the case in which the HAL may still hold, and still call, the listener, so the detach cannot be left to the close that failed. The postcondition holds on every path through the destructor.
- The destructor takes the instance lock and then calls close(), which takes it again; that is sound because `CCEC_OSAL::Mutex` is recursive, and it is the legacy destructor's own shape, preserved rather than tidied.
- The detach takes the listener's lock while the instance lock is held. The nesting is one-directional: a callback never acquires the instance lock, because it reaches the owner only through getIncomingQueue(), which takes no lock.

### `CCEC::DriverAidlImpl::close()`

- The silent return when not OPENED has the same cause as open()'s: the legacy `throw InvalidStateException()` is `#if 0`'d out.
- The NULL sentinel wakes a blocked reader so it unwinds, and it is offered before the HAL transaction, which is the observable legacy order. It shares the incoming queue's 32 slots with received frames, so a full queue drops it, as on the legacy path.
- The failure is raised only after the state is CLOSED, so a caller that swallows the exception still sees a consistently closed object; DriverImpl::close() likewise sets the state before raising on an `HdmiCecClose()` failure. `IOException` is raised for a non-ok binder status or for success with a false result.
- The local logical-address list is not cleared because DriverImpl::close() does not clear it either, so isValidLogicalAddress() can remain true across a close on the legacy path, and the HAL removes the addresses on its own side; clearing would be an unauthorized improvement.
- A failed close gives no such removal: the HAL may still hold the registered address. The failure arm therefore writes the local entry, when there is one, to `unconfirmedReleaseAddress` before it raises, and leaves an existing record alone when the list is empty. The next open()'s registration releases that address before it allocates, so a HAL that accepts the re-open while keeping the old address never ends up holding two.
- Safe on a closed instance, which returns silently.
- The listener detach sits before the failure check and must stay there. A failed `IHdmiCec::close()` leaves the HAL entitled to keep calling the listener, because the AIDL no-further-callbacks guarantee attaches only to a close that succeeded; detaching only on the success arm would leave a listener holding a back pointer into an owner about to be destroyed (CWE-416).
- Blocked item B2: `IHdmiCec.close()` is a high-confidence candidate for the legacy `HdmiCecClose()`, pending confirmation by the HAL mapping-table owners, whose table has no entry for `HdmiCecClose()`. The candidate is used because a back-end that cannot close is not deliverable — every `LibCCEC::term()` would leak an open session and the next open() would fail with `EX_ILLEGAL_STATE`. If the owners reject it, only this method's body changes.
- A stall past the threshold is reported naming `IHdmiCec::close`.

### `CCEC::DriverAidlImpl::read()`

- There is no AIDL call in read(), which is precisely what leaves the Bus reader thread unchanged by the migration; only the thread that produces into the queue differs.
- The body, flush loop included, is the legacy body with only the class name changed. The flush dereferences every entry it dequeues, exactly as `DriverImpl::read()` does, so a second NULL sentinel queued behind the first (close() offers one per transition out of OPENED) faults on the Bus reader thread on both back-ends. That inherited defect is recorded as the repeated-restart risk in the Project Guide; its repair adds the null check to both flush loops in a separately scoped change.
- The `frame` out-parameter is also written by the flush that precedes the raise on close, exactly as on the legacy path.
- Superseded: an earlier revision null-checked every entry in the flush, a departure from the legacy body that was withdrawn.
- read() is called from the Bus reader thread, never from a plugin thread.

### `CCEC::DriverAidlImpl::write()`

- Statement order as legacy: the frame buffer is taken and the frame logged, then the lock is acquired and the state checked, then the transmit runs with the lock still held.
- The status translation respects an inverted sense: `ACK_STATE_0` means acknowledged for a directed message but rejected for a broadcast, and `ACK_STATE_1` is the mirror. The destination nibble is read from `frame.at(0) & 0x0F` exactly as the legacy implementation reads it, and the CEC CTS 9-3-3 arm — a rejected broadcast `REPORT_PHYSICAL_ADDRESS` — raises so the caller retries.
- The translation has one arm per documented `SendMessageStatus` value. The result is an `int32_t` the generated proxy reads out of a parcel, so a HAL can return a value outside the three enumerators; such a value is logged by number at `LOG_EXP` and returns normally, as an unrecognised status does on the legacy back-end. *Superseded:* an earlier revision raised `IOException` for it so that a suppressed frame could not read as delivered; that raise was an observable difference outside the authorized list and was removed.
- Exceptions in full. `IOException`: a non-ok `sendMessage()` binder status; `BUSY`, meaning arbitration failed and nothing was sent; a frame longer than `AIDL_MAX_MESSAGE_LENGTH` (16); or no controller session held. `CECNoAckException`: a directed message was not acknowledged, or the CTS 9-3-3 broadcast arm was hit. `InvalidStateException`: not OPENED. `std::out_of_range`: the frame has no header byte — the prelude decodes that byte before the state guard and the formatter catches only the CCEC Exception family, and the status translation reads the same byte again later; both readings match legacy, which runs the same prelude in the same order, so an empty frame raises this rather than `InvalidStateException` even on a closed driver.
- A normal return means no arm of the legacy status mapping was triggered, not that every receiver accepted the frame: a broadcast the HAL reports as `ACK_STATE_0` returns normally for every opcode except the CTS 9-3-3 `REPORT_PHYSICAL_ADDRESS` arm, because that is what the legacy mapping does.
- `sendMessage()` states a 16-byte maximum while CECFrame carries up to `CECFrame::MAX_LENGTH` (128) and the legacy HAL specification allows 20, so a 17- to 20-byte frame is sendable on legacy and raises `IOException` here. The frame is policed, never truncated, since truncating would put a corrupt CEC frame on the bus. No frame the current plugin surface produces comes close to the limit.
- The instance lock is held across the whole IPC round trip, exactly as legacy holds it across the in-process call, preserving `HdmiCecTx` serialization rather than introducing new contention. A stalled transmit therefore also holds off every other operation that takes the instance lock, and narrowing that critical section is not an option. getPhysicalAddress() takes no lock and still answers 1.0.0.0 while a transmit is stalled (see "No HAL call" under "Physical address (AIDL back-end)"), and the receive callback, which never takes the instance lock, can still queue frames until the incoming queue is full. A stall is reported naming `IHdmiCecController::sendMessage`.

### `CCEC::DriverAidlImpl::writeAsync()`

- The AIDL HAL exposes no asynchronous transmit and none is emulated: no threads, no work queue, no deferred callback and no wrapper. This is a decided point, not an unresolved mapping.
- It is safe because every plugin transmit goes through `Connection::sendToAsync()` and `Connection::sendAsync()` onto the Bus writer thread, which calls the synchronous write().
- DriverImpl::writeAsync() takes the frame buffer and logs the frame before it locks and checks the state; this override runs the same two prelude calls and the same guard, then declines. `std::out_of_range` (empty frame, on both back-ends), `InvalidStateException` (not OPENED) and `OperationNotSupportedException` (always, once prelude and guard pass) are the three outcomes; the closed-driver and empty-frame cases behave identically on both back-ends.

### `CCEC::DriverAidlImpl::removeLogicalAddress()`

- The legacy shape is the specification: the guard, the local removal, and only then the HAL call, whose result legacy ignores. Both a false result and a non-ok binder status are logged and otherwise ignored, because raising where legacy returns silently would be an unregistered behaviour change.
- The address is marshalled as a one-element `std::vector<int32_t>`; no multi-address state is introduced.
- Nothing is reported about HAL-side failure, by design; a caller that needs to know an address was released must re-query.
- The local removal forgets the address while the HAL may still hold it, so the address this back-end held is written to the private `unconfirmedReleaseAddress` record before the local removal, and only an ok `true` release clears it. A release that reports false or fails is logged and ignored, and an exception from the request allocation or the proxy propagates; each leaves the record set. The next addLogicalAddress() settles that record before adding anything; a successful close() clears it, and the next open()'s registration settles it before allocating.
- A stall is reported naming `IHdmiCecController::removeLogicalAddresses`.

### `CCEC::DriverAidlImpl::poll()`

- The AIDL `getState()` is deliberately not used: a poll is a CEC ping performed by a one-byte transmit, not a state query, so the outcome reaches the caller through write()'s exceptions. `CECNoAckException` is the normal way a caller learns the address is free.
- Because the frame is handed to write(), the poll travels over whichever transport the enclosing back-end uses.
- poll() issues no AIDL call of its own, which is why the bounded-response prerequisite is restated on it: a caller reading only the exception list would take the three exceptions for the complete set of outcomes. A stall is logged naming `IHdmiCecController::sendMessage`, not poll(), and because write() holds the instance mutex across the transmit, a stalled poll holds off every other operation that takes that mutex; getPhysicalAddress() takes none and still answers 1.0.0.0.

### `CCEC::DriverAidlImpl::printFrameDetails()`

- The header is decoded, the opcode name is resolved when the frame is long enough to carry one, and every CCEC exception raised while decoding a malformed frame is caught and logged, so diagnostics can never break a transmit.
- An empty frame is not contained, and containing it would change observable behaviour: the formatter's only handler names `Exception`, the CCEC base, while the header decode of an empty frame raises `std::out_of_range`, outside that family. write() and writeAsync() call this ahead of their state guard, so an empty frame raises `std::out_of_range` out of them rather than `InvalidStateException` on both back-ends.
- Declared `noexcept(false)` to match the Driver interface; it propagates `std::out_of_range` and no CCEC exception.

### `CCEC::DriverAidlImpl::BinderPreflightProbe` (forward declaration)

- Declared ahead of isServiceAvailable() and defined further down, next to the predicate that consumes it; a reference to an incomplete nested type is all the declaration needs.

### `CCEC::DriverAidlImpl::isServiceAvailable()`

- Asked once by the selection helper in `ccec/src/Driver.cpp`, after both back-ends are constructed. Four ordered stages, stopping at the first that fails:
  1. isBinderPreflightOk() on the driver path, so nothing in the process touches libbinder unless doing so is known to be safe. Custody of the validated descriptor and its identity is taken here and held for the rest of the method.
  2. Custody re-verification immediately before the lookup: the same path is resolved again, its identity compared against the one the preflight validated, and the context manager asked again under the same bound. This narrows, but cannot close, the window between the check and libbinder's own use of the node and the manager (see `DriverAidlImpl::BinderNodeIdentity` and **Residual acquisition window** below).
  3. The lookup of `IHdmiCec::serviceName()` — the literal `"HdmiCec"` — through the binder service manager, yielding a typed proxy or nullptr.
  4. The compatibility check, which rejects a null proxy, an empty or `"-1"` interface hash, an unfrozen development server, and a server whose interface version does not satisfy the compiled-against `IHdmiCec::VERSION` within the same era and major.
- Presence alone is not sufficient: a service that answers but cannot be spoken to compatibly is treated as absent and the legacy back-end is selected. The `halcompat` rule is reused as it stands; it accepts a server newer than this client within the same era and major. A compatible proxy is cached for open(), so the lookup happens once.
- `binderDriverPath` is a parameter for the same reason as isBinderPreflightOk()'s: the declining arms can be exercised without rendering a runner's real driver unusable. It defaults to `DEFAULT_BINDER_DRIVER_PATH`, so the production call site names nothing; `contextManagerTimeoutMs` defaults to `DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS`; `probe` carries the six kernel-facing operations and defaults to defaultBinderProbe(), the real syscalls.
- A true result means the preflight passed, the node was unchanged at the last comparison before the lookup, the service resolved, and it is compatible. A false result requires the caller to select the legacy back-end.
- Safe to call on a platform with no binder driver, no `servicemanager` and no AIDL HAL — the purpose of the preflight. On a true result, and only then, the proxy is held. The descriptor the preflight retained is released on every exit path, including the throwing one, so the custody window closes in this method.
- It never propagates an exception: a function-level catch-all turns one into a decline. Every negative probe outcome — no node, a wrong node, a protocol mismatch, no context manager answering within the bound — is a bounded, nonfatal decline, because a legacy-only SOC must reach the legacy back-end rather than fail to initialize. The bound does not extend past the last probe; see **Residual acquisition window** below. It is not thread safe and need not be: it is called once from the factory's one-time static initializer, which the language serializes.
- **Service-authorization prerequisite.** The platform must enforce service-manager add and find authorization for `"HdmiCec"`, so that only the genuine HDMI CEC HAL may register the name and only authorized clients may resolve it, and must apply restrictive ownership and mode to the binder node so an unprivileged process cannot reach the driver. Nothing in the middleware makes a platform that does neither safe.
  - The pinned binder SDK does not meet this on its own. In `linux_binder_idl` 2.6.0, `cmds/servicemanager/Access.cpp` guards its `selinux_check_access()` call with `#ifdef __ANDROID__` and otherwise returns true, so on a Linux port `canAdd()`, `canFind()` and `canList()` allow everything and any process able to open the binder node may register `"HdmiCec"`. Supplying the authorization is platform-image work — an SELinux-enabled daemon build with a policy that labels this service, or an equivalent restriction on who may reach the binder context — and cannot move into this middleware, which is a client.
  - `IServiceManager::isDeclared()` is not a stand-in: on the same pin `cmds/servicemanager/ServiceManager.cpp` answers it from `isVintfDeclared()` only under `#ifdef __ANDROID__` and otherwise reports false, so a declaration check would decline every service on every conformant platform.
  - The consequence: the predicate identifies a service by the generated service name plus the interface hash and version the frozen AIDL snapshot compiles in, all publicly reproducible constants. On a platform that lets any process register `"HdmiCec"`, a hostile local process can register first, be selected as the HAL, and then observe every outbound CEC frame, suppress or alter transmits, report logical addresses of its choosing, and inject inbound frames through the listener handed to it at open(). Selection is resolved once per process, so the substitution persists for the process lifetime.
  - What the back-end does check: the node must exist, be a character device and be owned by UID 0; its kernel protocol version must equal the one the linked libbinder was built for; all five captured identity attributes — device, inode, rdev, mode and uid — are re-verified immediately before the lookup, so a substitution, `chmod` or `chown` before that comparison is declined (one after it, before libbinder's own open, cannot be detected; see `DriverAidlImpl::BinderNodeIdentity`); and the resolved service must present a compatible interface hash and version.
  - These checks establish that the transport is the platform's genuine binder driver and that the peer speaks this interface revision. None authenticates the peer, and none can: `BpBinder`, the proxy type the lookup yields, exposes transact, liveness, death linkage and object attachment but no peer identity; `IPCThreadState::getCallingUid()`/`getCallingPid()`/`getCallingSid()` describe an inbound transaction being served, which a client performing a lookup does not have. `IServiceManager::getServiceDebugInfo()` reports the daemon's record of the registering pid; that is evidence for a log, not a gate — unauthenticated registrar bookkeeping that says nothing about entitlement and is reachable only after `defaultServiceManager()` has opened the driver.
  - Deliberately not added, and not to be added in the name of fixing this: no HAL method (the AIDL surface is consumed as it exists), no public middleware selector or override (the public API does not change), and no vendor, variant or configuration conditional on the selection. The selection rests on runtime service availability alone, and a conditional would be a second, weaker authorization mechanism a hostile process on a misconfigured platform could satisfy as easily.
- **Bounded-response prerequisite.** Once a synchronous binder transaction is entered, no client-side deadline exists. The platform must start `servicemanager` before the middleware and keep it answering, and must guarantee that the HDMI CEC HAL answers every transaction within a bound; a platform that cannot bound the HAL must not register `"HdmiCec"`, which makes this predicate decline and the middleware select the legacy back-end.
  - The middleware cannot substitute a bound, because of the pinned libbinder: the C++ backend exposes no per-transaction timeout; `IPCThreadState::talkWithDriver()` retries on `EINTR`, so neither a signal nor an interval timer can break a blocked transaction out; `linkToDeath` detects a dead service, not a hung one, and death recipients are omitted because the legacy in-process HAL had no counterpart; a watchdog thread could not cancel a blocked transaction either and is barred as a new abstraction; and narrowing write()'s critical section is barred because holding the instance mutex across the transmit preserves legacy `HdmiCecTx` serialization.
  - What is bounded: the driver-node checks and the context-manager pings in both stages above have hard deadlines, so a `servicemanager` that is absent, or wedged when either ping is made, becomes a decline rather than an unbounded wait.
  - **Residual acquisition window — unresolved.** Nothing after the second ping is bounded. `halcompat::getService<IHdmiCec>()` calls `defaultServiceManager()`, which on the pin retries `getContextObject()` once a second without limit until handle 0 resolves (`libs/binder/IServiceManager.cpp`), then `checkService()`; `halcompat::isCompatible<IHdmiCec>()` then reads `getInterfaceHash()` and `getInterfaceVersion()`, and a rejection's diagnostic reads both once more. Each is a synchronous transaction that `IPCThreadState::waitForResponse()` waits on with no timeout. A `servicemanager` that dies after the second ping and before handle 0 resolves, one that wedges after that ping, or a registered HAL that stops answering therefore blocks `LibCCEC::init()` without bound. The two pings narrow this window; no change permitted to this middleware closes it, since `halcompat.h` and the SDK are consumed read-only and no HAL method, watchdog, wrapper or second selection mechanism may be added.
  - Owners: the binder SDK pin owner, for a client-side transaction deadline or a bounded lookup in `linux_binder_idl` or `halcompat.h`; the platform integrator, for `servicemanager` liveness and the HAL response bound. The Project Guide tracks it with the client-side transaction deadline obligation.
  - Every synchronous AIDL call measures its elapsed time and, past `SLOW_HAL_CALL_WARN_MS` (1000 ms, in `DriverAidlImpl.cpp`), logs a report naming the exact method and the elapsed time; that report is the whole of what the middleware can do about a stall.

## ccec/src/DriverAidlImpl.hpp (part 2 of 2)

Detail moved out of the Doxygen comments from `unavailabilityReason()` to the end of the header.

- Superseded statements: one, unrelated to addresses. The `status` member comment described an `std::atomic<int>`; the member is now a plain `int` (see `DriverAidlImpl::status`). No comment in this part described the pre-refine logical-address or physical-address behaviour. The `logicalAddresses` member comment belongs to the one-address-per-device change and is not covered here.

### DriverAidlImpl::unavailabilityReason()

- The decision is kept, not reconstructed. isServiceAvailable() already knows which of its four ordered stages declined. The selection helper in `ccec/src/Driver.cpp` needs that answer to name the platform condition in its fallback log line, so it reads the record instead of asking again.
- Asking again would be wrong for two independent reasons:
  - Re-running the bounded preflight pays the context-manager timeout a second time. That doubles the worst-case delay inside `LibCCEC::init()`, on exactly the platform least able to afford it.
  - The second run need not agree with the first. A `servicemanager` that starts or dies between the two calls flips the verdict, so the reported reason could name a condition that did not cause the fallback.
- `NULL` means there is no fallback reason to report, and a caller must not print one. A non-NULL value is a stable, human-readable phrase for substitution into a log line. It has static storage duration and outlives the object.
- Precondition: none. It is safe to call before isServiceAvailable() has ever run. Postcondition: nothing changes; this is a pure read of state the query already set.
- The phrases are a test and tooling contract. The coverage runner and the L2 tier both transcribe the resulting log lines, so rewording a phrase breaks those consumers.
- See also `Driver::getInstance()`.

### DriverAidlImpl::BinderNodeIdentity

- **Why it exists.** The preflight opens the configured path and checks that the node behind it is a binder driver of the right protocol with an answering context manager. During the service lookup, libbinder then opens the same pathname again on its own. These are two separate resolutions of one name. On the pinned stack the second one is fatal when it goes wrong: a failed driver open or a protocol mismatch is a `LOG_ALWAYS_FATAL_IF` abort. A node swapped between the two resolutions would turn a check that exists to guarantee a nonfatal legacy fallback into a process abort. This is a time-of-check-to-time-of-use window with the worst possible consequence.
- **What the comparison does.** The identity of the validated node is carried out of the check and compared again immediately before use. This narrows the window to its structural minimum but does not close it.
- **What each attribute catches.** All five attributes are compared.
  - `device` and `inode` together identify one filesystem object uniquely.
  - `rdev` is the driver's major/minor pair. A replacement node, whether created, bind-mounted or reached through a re-pointed symlink, differs in at least one of `device`, `inode` and `rdev`.
- **Why the descriptor stays open.** The preflight holds the validated descriptor open across the window. That is what makes the inode comparison meaningful: an open descriptor pins the inode, so its number cannot be recycled by a replacement created at the same path.
- **Why `mode` and `uid` are both validated and compared.** Each is validated once against fixed requirements (character device, owned by root) and then compared again across the window. The validation shows the node was a legitimate, privileged, kernel-published object when it was checked. The comparison shows that nobody ran `chmod` or `chown` on that same inode afterwards. Without the comparison, an attacker who cannot replace the node could still widen write access to it, or give it to an unprivileged owner, before libbinder opens the name. A `(device, inode, rdev)` comparison would report that as unchanged, because the object really is the same object.
- **No particular mode or owner is required by the comparison**, only that neither changed. It is therefore correct at any platform baseline: a restrictive node (0600) and a broadly accessible one (0666) both pass unchanged. AOSP-derived layouts publish 0666 deliberately because every binder client must be able to open the node.
- **What is not closed.** The comparison is the last statement before the lookup. Even so, libbinder still resolves the pathname independently inside the lookup and cannot be handed this descriptor. In `linux_binder_idl` 2.6.0:
  - The only driver entry points are `ProcessState::self()` and `ProcessState::initWithDriver(const char*)`, and both take a pathname.
  - The descriptor is the private `ProcessState::mDriverFD`, which has no accessor and no injector.
  - `open_driver(const char* driver)` in `libs/binder/ProcessState.cpp` calls `open(driver, O_RDWR | O_CLOEXEC)` on that pathname.
  - `ProcessState::init()` adds a third resolution: an `access(driver, R_OK)` probe that silently substitutes `/dev/binder` when it fails.

  A substitution made after the comparison and before libbinder's open, by a process privileged enough to replace a node under `/dev`, cannot be detected here. On the pinned stack such a bad open is a `LOG_ALWAYS_FATAL_IF` abort rather than a decline.
- **Considered and rejected: re-checking after the lookup.** It would make things worse. `gProcess` is a `[[clang::no_destroy]]` static created under `std::call_once`, and `~ProcessState()` is private. Once the lookup has run, the process is bound for its whole lifetime to whatever driver libbinder opened. A decline after the lookup would select the legacy back-end while leaving the process permanently attached to the substituted driver, which is worse than the abort it was meant to avoid.
- **Residual risk.** What remains is the platform's, and it is the ordinary custody assumption every Binder client on this stack already makes: `/dev` must be writable only by the privileged platform image. Within that assumption, a substitution, `chmod` or `chown` in the much larger interval before the comparison is caught and declined instead of being carried into libbinder's fatal open.
- **Plain integers.** Every member is a plain integer, and that is required. The struct names no binder kernel type and no `<sys/stat.h>` type, so the header still compiles where the binder kernel UAPI definitions are absent. This is the same reason `BinderPreflightProbe` carries the protocol version as `unsigned int`. The widths are the widest plain integers the values can need, so no host's `dev_t` or `ino_t` is truncated.
- **Not public API.** The type is internal and test-visible. `ccec/src/DriverAidlImpl.hpp` is not an installed header, so the type adds nothing to the middleware public API.
- See also `BinderPreflightProbe::identifyDescriptor` and `BinderPreflightProbe::identifyPath`.

### DriverAidlImpl::BinderNodeIdentity::mode

- The file-type bits are extracted with `BINDER_NODE_MODE_TYPE_MASK` and validated once inside the preflight.
- The whole value is compared across the check-to-use window, so a `chmod` of the validated inode cannot pass as unchanged.
- Permission bits broader than owner-only are observed and reported at `LOG_INFO`. This is a diagnostic and never a verdict; see `isBinderPreflightOk()`.

### DriverAidlImpl::BinderNodeIdentity::uid

- The value is compared against `BINDER_NODE_REQUIRED_OWNER_UID` inside the preflight.
- It is compared against the validated value again across the check-to-use window, so a `chown` of the validated inode cannot pass as unchanged.

### DriverAidlImpl::BinderPreflightProbe

- **Why the seam is needed.** Without it the preflight cannot be fully verified. Most of the predicate's decision arms cannot be reached through its path argument alone on any host:
  - a node that opens and reports a mismatched protocol version;
  - a node that reports a matching version and then fails the context-manager check;
  - a node whose identity changes between the check and the use;
  - a fully positive verdict.

  Through a real path a driverless host reaches only early refusals: an absent node fails the open (decision point 2), a regular file opens and is refused at the character-device check (decision point 4), and nothing short of a binder driver gets past the protocol read (decision point 6). A binder-capable host reaches only the positive arm. Without the seam, the most consequential arms would ship unexercised: the protocol-equality check, which stands between a mismatched kernel and libbinder's abort, and the identity re-check, which stands between a substituted node and the same abort. Substituting the six operations makes every arm reachable deterministically, with no binder driver and without making a runner's real driver unusable.
- **Why function pointers.** The struct is POD with function pointers rather than an abstract interface. It adds no virtual dispatch, no allocation and no ownership question, and it keeps the default (the real syscalls) a compile-time constant.
- **No binder kernel types.** Its members mention no binder kernel type, which is required: the header must still compile where the binder kernel UAPI definitions are absent. The protocol version therefore crosses the boundary as a plain `unsigned int` and the driver node as a plain descriptor.
- **Not public API.** It is internal and test-visible because the tests build synthetic probes from it at namespace scope, which a private or protected nested type would forbid. Nothing outside `ccec/src` and the test suites may use it.
- **Members must be non-null.** `isBinderPreflightOk()` calls every member unconditionally. It does not defend against a partially filled probe, just as it does not defend against a null `::open`.
- See also `expectedBinderProtocolVersion()`.

### DriverAidlImpl::BinderPreflightProbe::openNode

- It is declared with a fixed second parameter instead of as the variadic `::open` itself, so the default probe supplies a thin wrapper.

### DriverAidlImpl::BinderPreflightProbe::identifyDescriptor

- The identity is taken from the descriptor, not the path, on purpose. The answer then describes the object this process has open, which cannot change underneath it. That makes it usable as the reference the later re-check compares against.
- See also `BinderNodeIdentity`.

### DriverAidlImpl::BinderPreflightProbe::identifyPath

- Following symlinks is required, not incidental. A binderfs deployment may publish `/dev/binder` as a symlink to `/dev/binderfs/binder`, and the identity returned must be that of the node libbinder will open when it resolves the same name.
- A `-1` result means the path could not be resolved or stat'ed.
- See also `BinderNodeIdentity` and `isServiceAvailable()`.

### DriverAidlImpl::BinderPreflightProbe::readProtocolVersion

- The version is passed out as an `unsigned int` so that no binder kernel type appears in this header.

### DriverAidlImpl::BinderPreflightProbe::pingContextManager

- The real implementation posts a `PING_TRANSACTION` to handle 0 over the descriptor and drains the driver's command stream until the deadline.
- One call per descriptor. The real implementation maps the driver's transaction buffer, and the binder driver allows exactly one mapping per open descriptor for that descriptor's lifetime. Unmapping does not restore the right to map again, so a second call on the same descriptor fails whatever the platform's state. A caller that needs to ask twice must use a second, independently opened descriptor.
- See also `isServiceAvailable()`.

### DriverAidlImpl::BinderPreflightProbe::closeNode

- The preflight ignores the return value, just as the production code it replaces ignored the return value of `::close`.

### DriverAidlImpl::defaultBinderProbe()

- It is the default argument of `isBinderPreflightOk()`. Every production call site names nothing and still gets the real kernel-facing operations, including every log line the predicate emits.
- It returns a reference to a single immutable instance with static storage duration. That is why it is safe as a default argument and why callers own nothing.
- Precondition: none.
- The instance is const, so no caller can rebind the probe production uses. A test supplies its own probe by passing it explicitly.
- See also `BinderPreflightProbe`.

### DriverAidlImpl::expectedBinderProtocolVersion()

- It is exposed as a function so a test can build both a matching and a mismatching version without the binder kernel UAPI definitions leaking into this header or into the test translation unit.
- The constant follows `BINDER_IPC_32BIT`, which is why it is read from the build rather than written down anywhere.
- When it returns 0, the preflight cannot reach its comparison, because the default probe's version read fails first.
- Precondition: none.

### DriverAidlImpl::isBinderPreflightOk()

- **Why a preflight is required.** Reaching the service manager directly is unsafe in two independent ways on the pinned binder stack. Either one defeats the absolute requirement that a supported SOC without an AIDL HAL reaches the legacy back-end:
  - A missing or protocol-mismatched driver node is fatal, not an error return. The pin extends libbinder's failed-driver `LOG_ALWAYS_FATAL_IF` to plain Linux, so a check that upstream guards for Android only is live here. It would abort the middleware during initialization instead of falling back.
  - A missing context manager blocks indefinitely. Obtaining an `IServiceManager` polls once a second until binder handle 0 resolves, with no upper bound. A working driver with no running `servicemanager` would stall `LibCCEC::init()` forever.
- **The checks, in order, stopping at the first failure:**
  1. The driver node exists and can be opened.
  2. The node the descriptor refers to can be identified at all.
  3. It is a character device, because every supported binder layout publishes one.
  4. It is owned by root. Every supported layout creates it as root, and a node owned by anyone else is one an unprivileged process could substitute.
  5. The protocol version it reports equals the version the linked libbinder was built for. libbinder enforces equality, and a mismatch fails every open.
  6. Binder handle 0 (the context manager) resolves within a bounded timeout instead of being waited on without limit.

  Only when all of them pass may the caller go on to the service lookup and the compatibility check. Any failure means "AIDL absent" and the legacy back-end is used.
- **Observed but not required: permissive node mode.** A node writable beyond its owner is logged once at `LOG_INFO`, with its permission bits, and the verdict is unchanged. It is not a warning, because every standard binder node is `0666` and a WARN on every healthy start would teach integrators to ignore WARN. The middleware cannot require restrictive permissions, because every client process that uses binder must be able to open the node. AOSP-derived platforms publish it broadly accessible by design, and a binderfs deployment takes whatever mode binderfs assigns. Refusing a permissive mode would decline the AIDL path on conformant platforms while passing in a root-only CI guest. The enforceable node-level controls are therefore:
  - the character-device and root-owner checks;
  - the five-attribute identity comparison the caller makes just before the lookup, which catches the mode or owner changing inside the check-to-use window even though neither value is dictated.
- **Who may register `"HdmiCec"`.** Who may register and resolve the name is decided by `servicemanager` add and find policy in the platform image. It is recorded as a hard prerequisite on `isServiceAvailable()`, and no client-side code substitutes for it.
- **Custody of the descriptor.** Validating a pathname and then letting libbinder resolve the same pathname independently is a time-of-check-to-time-of-use window. On the pinned stack, losing that race causes a process abort, not a wrong answer.
  - A caller that intends to go on to the lookup passes `retainedDescriptor` and `retainedIdentity`.
  - On a positive verdict the predicate does not release the validated descriptor. It hands over the descriptor and the identity of the node it validated, so the caller can re-verify the same name just before use and keep the node's inode pinned in the meantime.
  - A caller that passes neither gets the original behaviour: the descriptor is released before return. The harness's own preflight assertion and every negative-arm test call it this way.
- **Parameters in full:**
  - `binderDriverPath` is a parameter, not hard-coded, so the negative arms (a nonexistent path, or a path whose reported protocol differs) can be exercised without making a test runner's real driver unusable. Defaults to `DEFAULT_BINDER_DRIVER_PATH`.
  - `contextManagerTimeoutMs`: zero means do not wait at all, which is how the timeout arm is exercised. Defaults to `DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS`.
  - `probe` is injected so arms a path argument cannot reach become reachable: a node that opens but reports a mismatched protocol version, a node that reports a matching version and then fails the context-manager check, a node that cannot be identified or is not a root-owned character device, and a fully positive verdict. No real path on a driverless host produces any of these. Defaults to `defaultBinderProbe()`, the real syscalls, so production behaviour and every production log line are exactly what they would be without the seam.
  - `retainedDescriptor`: when non-null and the verdict is true, it receives the validated descriptor, still open, and custody passes to the caller, which must release it through `probe.closeNode`. It is set to -1 on every false verdict, so a caller never has to tell "not retained" from "stale". Defaults to `NULL`, which releases the descriptor inside the predicate.
  - `retainedIdentity`: written only when it is non-null, `retainedDescriptor` is also non-null and the verdict is true. It then receives the identity of the validated node, for the caller to compare against a fresh resolution of the same path just before use. It is untouched otherwise, including on a true verdict given without `retainedDescriptor`, because the identity is only meaningful while the retained descriptor pins the inode. Defaults to `NULL`.
- **Return values in full.** `true`: every check passed, and the descriptor is retained if and only if `retainedDescriptor` is non-null. `false` covers all of the following, which are told apart in the log rather than in the return value:
  - the path was empty;
  - the node is absent or cannot be opened;
  - the node cannot be identified;
  - the node is not a root-owned character device;
  - the protocol version could not be read, or differs;
  - handle 0 did not resolve within the bound;
  - the build carries no binder kernel ABI definitions to check any of this with.
- **Precondition:** none whatsoever. This is the first thing that runs, on any platform.
- **Postcondition:** nothing in the process is left initialized. No `ProcessState` singleton is created and no threadpool is started. The descriptor and mapping the check uses are released before it returns, so a false result leaves the process exactly as it was. The one exception is the custody window a caller opts into: on a true verdict the retained descriptor is still open, and closing it on every exit path is then the caller's obligation.
- **Why it is private, and how tests reach it.** It is a private static, as specified; its only production caller is `isServiceAvailable()`. The class befriends one name for the tests, `BinderPreflightTestAccess`, which production never defines. The two L1 translation units that call the predicate, `tests/L1Tests/test_main.cpp` and `tests/L1Tests/ccec/test_DriverAidl.cpp`, each define that struct token-identically, as the one-definition rule requires. Its one member template forwards its arguments unchanged, so the predicate's own default arguments apply and production carries no wrapper or forwarder. The friend names a test-only gateway, not a test fixture, and the gateway reaches nothing but the predicate. The header is not installed, so none of this adds to the middleware public API.
- **Guarantees.** Implementations must not propagate exceptions and must not block beyond the stated bound. The caller relies on both.
- **Build setting.** The protocol constant follows `BINDER_IPC_32BIT`, so the middleware must be compiled with the same setting as the libbinder it links. An all-32-bit platform speaks protocol 7; 32-bit middleware against a 64-bit vendor speaks 8. A mismatch makes every open fail, and this check reports that as "AIDL absent" instead of letting libbinder abort.
- See also `BinderPreflightProbe`, `defaultBinderProbe()`, `expectedBinderProtocolVersion()`, `DEFAULT_BINDER_DRIVER_PATH` and `DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS`.

### DriverAidlImpl::describeObservedInterfaceHash()

- It describes a single string: what the value is, never what it caused. `isServiceAvailable()` cannot know which of halcompat's three rules rejected a server. The metadata the predicate read is a local inside a read-only header, and a fresh read is a different binder transaction. That distinction is the reason this function exists instead of a classification chain.
- The hash is taken from a read made after the decision.
- Precondition: none. Postcondition: no state changes. No compatibility decision is taken on this value.
- Why it is public: the production arm that logs it cannot be reached without a binder transport, so a test call is the only way the wording is covered on a host without one.
- See also `isServiceAvailable()`.

### DriverAidlImpl::observedMetadataWouldBeAccepted()

- It answers one question about one snapshot: had these values been the ones halcompat read, would it have accepted them?
- A `true` answer does not overturn the rejection, because the decision stands on the metadata that governed it. It shows that the observation disagrees with the decision: the server's metadata changed or recovered, and the rejection must not be blamed on the version rule.
- `halcompat::detail::isCompatible()` is the real rule. It is constexpr over two ints, so it performs no transaction.
- The hash half mirrors halcompat's gate order for the observed value only. It does not accept an unfrozen server, because production calls the predicate with its `allowUnfrozen` default of `false`.
- Passing `hash` and `observedVersion` from two different reads is exactly the defect the signature exists to prevent. The precondition is that both come from one snapshot. Postcondition: no state changes.
- Why it is public: for the same reason as `describeObservedInterfaceHash()`.
- See also `isServiceAvailable()`.

### DriverAidlImpl::emitCompatibilityRejectionDiagnostic()

- It holds everything `isServiceAvailable()` says about a service that is present but incompatible, in one place. The wording a test captures is therefore byte-for-byte the wording production emits.
- **Why it is separate.** On a host with no binder driver the preflight declines first, so the compatibility stage is unreachable there. If the diagnostic were inline, its wording would go unverified exactly where it matters most. `isServiceAvailable()` is its only production caller. A second copy of the wording in a test would be the drift this arrangement exists to prevent.
- **Why it cannot name the rule.** The metadata the decision read are locals inside a template in a read-only consumed header, and a fresh read is a different transaction.
- The caller has already established that halcompat rejected the service (`halcompat::isCompatible<IHdmiCec>(service)` returned false).
- **The no-relabel guarantee is structural, not a promise.** The function is `static`, so it has no `this` and cannot touch `availabilityReason`. A failed observation therefore cannot relabel an established compatibility rejection as `REASON_QUERY_FAILED`. The outer handler that would do so is unreachable from here, because this function swallows its own failure.
- It returns `void`.
- Why it is public: for the same reason as `describeObservedInterfaceHash()`. On a host with no binder driver, a test call is the only way to exercise the real production wording.
- See also `isServiceAvailable()`.

### DriverAidlImpl access levels (public, protected, private)

- **Why `protected`, not `private`.** The receive-queue handoff and the lock that serializes the queue's producers must both be reachable from a test-local subclass. They cannot be reached any other way. The address-allocation helpers `logicalAddressCandidates()` and `registerDeviceLogicalAddress()` are protected for the same reason: the test subclass `AllocationProbe` drives them directly, because production reaches them only through `open()`, which needs a live AIDL service.
- **The invariant these members protect.** Every frame offered to the incoming queue has exactly one owner.
  - While the state is OPENED, the listener is the only producer of frames. It hands them over through `offerReceivedFrame()`, which refuses a frame when the queue is full instead of letting `EventQueue::offer()` discard it silently.
  - The NULL sentinel that `close()` offers wakes a Bus reader blocked in `EventQueue::poll()`. It shares the queue's 32 slots with received frames, so a full queue drops it, as the legacy queue does.
  - `close()` is also a producer, so it takes `queueProducerMutex` around its sentinel offer. A frame that passed the room check can then never meet a queue the sentinel filled in between, so the handoff's report always matches what the queue holds.
- **Why tests need this access.** Both halves of that contract need coverage on a host with no binder driver, and both are otherwise out of reach.
  - `offerReceivedFrame()` accepts a frame only while the state is OPENED. The state becomes OPENED only through `open()`, which requires a live, compatible AIDL service. `protected` lets a test-local subclass set up that precondition and drive the handoff directly, with no service and no driver. Tests reach the private `isBinderPreflightOk()` through the befriended `BinderPreflightTestAccess` for the corresponding reason: so its negative arms can be exercised.
  - `queueProducerMutex` must be reachable for a sharper reason. A serial test cannot observe a lock that is not taken: a case that fills the queue, offers once more and only then closes passes whether or not `close()` holds the lock. The lock is observed only by a test that holds it itself and drives the real `close()` from another thread. The sentinel offer cannot complete while the lock is held, so completing anyway is the regression. A case that re-implements `close()`'s offer instead of calling `close()` would pass even if production code stopped taking the lock, which is the one thing such a case exists to catch.
- **Considered and rejected:**
  - Reaching these members through the `friend`. The one friend, `BinderPreflightTestAccess`, serves only the static predicate, which needs no instance. These members need an object in a particular state, which the test-local subclass constructs and drives, so deriving reaches them without widening what the friend is for.
  - A public introspection or state-setting API would add real middleware surface, which the plan forbids.
- **Declaration order** matches `DriverImpl`, so the two back-ends diff cleanly against one another.
- **The predicate is private, as specified.** AAP §0.3.2.1 specifies the binder preflight predicate as a "private static" member of this class, and it is declared so, in its own `private:` section beside the rest of the preflight surface. The state and queue members that support the receive-path contract are `protected` instead of `private`. That choice is deliberate and recorded here instead of being left for a reader to discover from the class body. The specification owner decides whether to restate the requirement or accept this layout.
- **How the specified test route is met at `private`, and why the nested types stay public.** §0.3.2.1 also requires that a test translation unit reach the predicate through the relative-path include named in the header's file comment (`#include "../../../ccec/src/DriverAidlImpl.hpp"`). It must also pass a synthesized probe, so the negative arms can be exercised on a host with no binder driver.
  - The predicate is reached through `BinderPreflightTestAccess`, the one name the class befriends. Production never defines it; the two L1 units that call the predicate define it identically and forward each call unchanged.
  - The nested types the injected probe is built from must be nameable from outside the class, which befriending one gateway struct does not provide. This is a real constraint: `tests/L1Tests/ccec/test_DriverAidl.cpp` declares namespace-scope objects of type `DriverAidlImpl::BinderNodeIdentity`, and free functions whose parameter and return types are `DriverAidlImpl::BinderNodeIdentity *` and `DriverAidlImpl::BinderPreflightProbe`. None of these declarations could name the types if they were protected or private, because they are not members of any subclass.

  `public` on the two nested types is the cost of the specified test route.
- **What each access level holds** (the narrowest grouping that works):
  - `public`:
    - the twelve `CCEC::Driver` overrides, the constructor and the destructor, which are public in the interface this class implements and cannot be narrowed;
    - the lifecycle enum and the `IncomingQueue` typedef, which appear in those signatures and in the members;
    - the named constants;
    - `isServiceAvailable()` and `unavailabilityReason()`, which the factory in `ccec/src/Driver.cpp` calls from outside the class;
    - the test-reachable preflight surface: `defaultBinderProbe()`, `expectedBinderProtocolVersion()`, `BinderNodeIdentity` and `BinderPreflightProbe`;
    - the three static compatibility diagnostics, which take everything they use as parameters and hold no state.
  - `protected`: everything that carries instance state, the receive-path invariant or the address allocation. That is the `EventListener` class, `getIncomingQueue()`, `offerReceivedFrame()`, the address-allocation helpers `logicalAddressCandidates()` and `registerDeviceLogicalAddress()`, the lifecycle state, the incoming queue, both locks, the local address list, the two session proxies, the listener pointer and the recorded availability reason. These are reachable only by deriving, which the test-local subclass does and nothing in production does.
  - `private`: the copy constructor and copy assignment operator, declared and never defined; the `LOCAL_DEVICE_TYPE` constant, which no test needs to reach; and `isBinderPreflightOk()`, which tests reach only through the befriended `BinderPreflightTestAccess`.
- **Why the grouping cannot move.** Moving any protected member to private would remove the only coverage the receive-path contract and the address-allocation helpers have on a driverless host. Moving any public member to protected would break the Driver interface, the factory, or the namespace-scope test declarations above.
- **No public API at any level.** None of this adds to the middleware public API. `ccec/src/DriverAidlImpl.hpp` is not an installed header: like `DriverImpl.hpp`, it is absent from the `nobase_include_HEADERS` list in `hdmicec/Makefile.am`. Nothing in production derives from this class, and no consumer can reach a protected member. Nothing outside `ccec/src` and the test suites may use any of it.

### DriverAidlImpl::EventListener

- Only a forward declaration appears in the header. The definition lives in `DriverAidlImpl.cpp`, so the server-side base header is not pulled into every translation unit that includes this one. Defining a nested class out of line is well formed, and its non-public access makes clear that it is not part of any interface.
- It reaches the incoming queue through `getIncomingQueue()`, never through `Driver::getInstance()`. Resolving through the factory would bring back the legacy static's `static_cast<DriverImpl &>`, which is ill-typed once the factory can return this class.
- **Why the back pointer is nullable.** The listener's lifetime is not this class's to decide. The listener crossed the binder boundary in `IHdmiCec::open()`, so the HAL holds a strong reference of its own, and releasing ours need not destroy it.
- **Detachment.** Every path that ends a session detaches the listener: both arms of `close()`, the failure arms of `open()`, and the destructor. Detachment is synchronous with any callback already in flight. Without it, a HAL that keeps calling after a failed close would dereference a destroyed owner (CWE-416).

### DriverAidlImpl::getIncomingQueue()

- The guard is the load-bearing part. It rejects a receive callback that arrives during or after a close, and it triggers the listener's release of the frame it was about to enqueue. The listener must reach the queue through this accessor and never touch the member directly, or it would accept frames the legacy path rejects.
- The legacy signature takes a native handle that its own body never reads. It is dropped here because the AIDL back-end has no handle to pass.
- The exception is raised when the driver is closed or closing. Precondition: none, because the state check is the method's purpose.
- **Unlocked state read.** The state, a plain `int`, is read without holding the instance lock, reproducing the existing unlocked read in `DriverImpl::getIncomingQueue()` and its data race. This is a known pre-existing condition, kept deliberately rather than fixed, because behaviour preservation outranks code improvement here.
- See also `read()` and `DriverImpl::getIncomingQueue()`.

### DriverAidlImpl::offerReceivedFrame()

- It is the ownership-transferring counterpart of `getIncomingQueue()`, and the reason the receive path cannot lose a frame.
  - `CCEC_OSAL::EventQueue::offer()` returns void and silently discards its argument when the queue is at `INCOMING_QUEUE_CAPACITY`. A caller that offers and then forgets the pointer leaks one frame per event for as long as the queue stays full.
  - This method checks that there is room before it offers, and tells the caller which of them owns the frame afterwards.
- It reaches the queue through `getIncomingQueue()`, never through the member, so the opened-state guard still applies. A frame arriving during or after a close is rejected by an `InvalidStateException` propagating out of here, exactly as on the legacy path, and the caller's existing catch releases it.
- On a `true` return the caller must not delete the frame. A `false` return means the queue already held `INCOMING_QUEUE_CAPACITY` entries.
- Precondition: the frame is heap-allocated and owned by the caller. Postcondition: exactly one of the following holds:
  - the frame is queued and the method returned true;
  - the frame still belongs to the caller and the method returned false or threw.
- **Why a separate lock.** Producers are serialized on `queueProducerMutex`, a lock of their own, never the instance lock. The instance lock is held by `write()` across an entire synchronous IPC round trip. Reusing it here would tie frame delivery on a binder thread to transmit latency, and could deadlock the receive path behind a stalled HAL.
- **Why check-then-offer is sound.** Every producer takes this lock, and nothing weaker would do. This method and `close()`'s NULL sentinel offer are the only writers on the queue, and both hold `queueProducerMutex` for their offer. While it is held nothing can raise the occupancy, so an observed "there is room" cannot turn false. Consumers do not take this lock and keep removing frames, which only lowers occupancy and is harmless to the check.
- **No read-back.** Because the check cannot go stale, an offer made after it always lands, so the method reports acceptance without reading the occupancy again. A read-back could not decide the outcome anyway: a frame that took the last slot and a frame the queue discarded both leave it full, and a consumer can take the frame the instant the offer wakes it.
- **The sentinel shares the slots.** The method refuses only at `INCOMING_QUEUE_CAPACITY`, so received frames may fill every slot, as on the legacy path, and a `close()` against a full queue then has its sentinel dropped, as on the legacy path (see `INCOMING_QUEUE_CAPACITY`).
- **Not bounded-time.** The only wait this method removes is the capacity wait: it never blocks waiting for the queue to drain, because a full queue is a refusal. Everything else it does can block:
  - it takes `queueProducerMutex`;
  - the occupancy read and the offer take the queue's own lock inside `CCEC_OSAL::EventQueue`;
  - the offer appends to a `std::deque`, which may allocate;
  - the offer signals the queue's condition variable, which locks and broadcasts.

  A binder thread calling this can be delayed by any of these, and no time bound is claimed.
- See also `INCOMING_QUEUE_CAPACITY`.

### DriverAidlImpl::status

- A plain `int` holding `CLOSED`, `CLOSING` or `OPENED`, declared exactly as `DriverImpl::status` is. `open()` and `close()` write it under the instance mutex, and every guard but one reads it under that mutex.
- **The unlocked read.** `getIncomingQueue()`'s guard reads it without the lock, on a binder threadpool thread, while `open()` or `close()` may be writing it under the lock. That is a data race on a plain `int`: the same one `DriverImpl::getIncomingQueue()` has between the vendor HAL's receive thread and a closing thread, with a binder thread in the HAL thread's place.
- **Why it is kept.** It is a known pre-existing condition of the legacy back-end, preserved deliberately rather than fixed, because behaviour preservation outranks code improvement here; the legacy file is not touched.
- **Superseded, recorded only as such.** The member was formerly an `std::atomic<int>` so that the unlocked read was a defined atomic load, a departure from `DriverImpl::status` justified on the ground that undefined behaviour need not be preserved. That departure is removed, and with it the reasoning about the atomic's template argument, its memory ordering and why no lock was added.
- See also `DriverAidlImpl::getIncomingQueue()` and `DriverImpl::status`.

### DriverAidlImpl::nativeHandle

- The constructor sets it to 0, and it is never used to address the AIDL HAL, which is reached through the two interface proxies. It is kept so the two back-ends declare the same members in the same order and diff cleanly against one another.

### DriverAidlImpl::rQueue

- The queue is constructed with `INCOMING_QUEUE_CAPACITY` explicitly, not left at the OSAL default, although both are 32. The capacity `offerReceivedFrame()` checks against is therefore the capacity the queue enforces. That equality lets the receive path prove, from its check alone, that its offer cannot be the one the queue silently discards.
- It holds received frames and `close()`'s NULL sentinel in the same 32 slots, as DriverImpl's queue does.

### DriverAidlImpl::queueProducerMutex

- Holding this lock in both writers is what makes the receive path's occupancy check meaningful. A producer that skipped the lock could raise the occupancy between that check and that offer. `EventQueue::offer()` would then discard the received frame silently while the receive path reported it accepted.
- **Why it is separate from the instance mutex.** `write()` holds the instance mutex across an entire synchronous IPC round trip; that is the legacy critical section, preserved on purpose. Taking it on the receive path would make frame delivery on a binder thread wait out every transmit and would put the receive path behind a stalled HAL.
- **Hold time.** The hold is short by comparison but not bounded. It spans the occupancy read and the offer after it, each of which takes the queue's own lock. The offer may allocate as it appends, and then locks and broadcasts the queue's condition variable. The lock covers no IPC and no logging.
- **Lock order.** `close()` takes this lock inside the instance lock. `offerReceivedFrame()` takes it alone and never takes the instance lock.
- See also `close()`.

### DriverAidlImpl::eventListener

- It is kept here so the HAL's strong reference has a local counterpart, and the object cannot be destroyed while the HAL may still call into it.
- Releasing this reference alone would not be enough. Releasing it need not destroy the object, and a survivor that has not been detached is a use-after-free waiting for the next callback.

### DriverAidlImpl::availabilityReason

- The selection helper reads it back through `unavailabilityReason()`. When the AIDL back-end is usable it is set to NULL. Otherwise it is set to the phrase naming the stage that declined.
- **Why `const char *`.** It is a `const char *` instead of a copied string on purpose. Every value ever stored is a string literal with static storage duration. There is nothing to own and nothing to allocate on a path that must not fail, and the pointer stays valid for the lifetime of the process.
- The constructor initializes it to NULL, which is what makes `unavailabilityReason()` safe to call before any query has run.

### DriverAidlImpl copy constructor and copy assignment operator

- Copying would duplicate the session proxies, the frame queue and the lock, none of which has copy semantics.
- The parameter exists only to suppress the implicit copy constructor and copy assignment operator. Copy assignment is disallowed for the same reason as copy construction.

## ccec/src/DriverAidlImpl.cpp (part 1 of 2)

Detail removed from the condensed comments in the first part of `ccec/src/DriverAidlImpl.cpp`:
the file header, the file-scope constants and helpers, `EventListener`, the constructor, the
destructor and `close()`.

- **Superseded statements.** The pre-refine file header stated three things that no longer hold.
  It said the back-end had a closed list of exactly three observable differences from the legacy
  back-end: the 16-byte frame limit in `write()`, the `OperationNotSupportedException` in
  `writeAsync()`, and the coarser failure category in `addLogicalAddress()`. It said B1 blocked
  `getPhysicalAddress()`. It said `DriverImpl.cpp` could stay the only production include of
  `hdmi_cec_driver.h` "only because `getPhysicalAddress()` reports its B1 block".
  What holds now:
  - The three differences remain, alongside two more:
    - `open()` discovers the logical address for the device's DeviceType and registers it with
      `addLogicalAddresses`. The back-end never registers more than one address: a replacement
      is added only once the HAL confirms the old one released. `getLogicalAddress()` reads the
      address back through `IHdmiCec::getLogicalAddresses()`.
    - `getPhysicalAddress()` always reports 1.0.0.0 (`0x01000000`) and makes no AIDL call.
  - B1 no longer blocks this back-end.
  - B2 (the `close()` mapping) is still pending owner confirmation.

### DriverAidlImpl.cpp (file header)

- **What the file contains.** All of the AIDL back-end:
  - the twelve `CCEC::Driver` overrides;
  - the runtime service-availability query that the selection point in `ccec/src/Driver.cpp`
    asks;
  - the binder preflight predicate that makes that query safe to attempt;
  - the nested event listener.
- **Parity is semantic, with the departures listed below.** Each override keeps its
  `DriverImpl` counterpart's guards, exceptions and statement order, with AIDL calls in place of
  the C HAL calls. Each AIDL call is preceded by a null-proxy check that takes the method's
  existing failure path, and followed by a slow-call warning (`warnIfHalCallSlow()`).
- **Copied bodies** (compared with the class name substituted):
  - `read()`, `isValidLogicalAddress()` and `printFrameDetails()` are byte-for-byte copies and
    call no HAL. `read()` keeps the legacy flush loop unchanged, so the Bus reader thread sees
    the same receive path on both back-ends.
  - `poll()` differs only in whitespace. It calls no HAL directly: its one-byte frame goes
    through `write()`, which calls `sendMessage()` (legacy: `HdmiCecTx()`).
  - `isValidLogicalAddress()` walks the list the same way, but on this back-end the list can also
    hold the address registered when the driver is enabled.
- **Departures**, by method:
  - `open()`: after the legacy state guard it raises `IOException` if no service proxy is held,
    calls `startThreadPool()`, then calls `IHdmiCec::open()` with the event listener, which
    replaces both legacy callback registrations. A non-ok status or a null controller detaches
    the listener and raises `IOException`. After OPENED, `registerDeviceLogicalAddress()` first
    releases any address `unconfirmedReleaseAddress` records (after a failed close, for one),
    then polls the candidate addresses for `LOCAL_DEVICE_TYPE` and registers the first free one,
    at most one, through `addLogicalAddresses()`. It catches and logs every failure; only thread
    cancellation's forced unwind escapes. See
    [Logical-address allocation and registration (AIDL back-end)](#logical-address-allocation-and-registration-aidl-back-end).
  - `addLogicalAddress()`: after the state guard and a no-controller `IOException`, an address
    above `0xE` raises `AddressNotAvailableException` before anything is released. Re-adding the
    locally recorded address returns true with no HAL call. Otherwise the held address (the local
    entry, else `unconfirmedReleaseAddress`) is removed first. A false or non-ok removal counts as
    a release only when `IHdmiCec::getLogicalAddresses()` succeeds and no longer lists the
    address. If it does not, the held address stays recorded, nothing is added and the call
    raises `IOException`, or `AddressNotAvailableException` when the HAL declined the release and
    still holds the address. Otherwise `source` is written to `unconfirmedReleaseAddress` and
    added. Success records it locally and clears the record; false raises
    `AddressNotAvailableException` and clears it; a non-ok status raises `IOException` and an
    exception propagates, both keeping it for the next add to settle.
  - `removeLogicalAddress()`: the legacy shape (state guard, local removal that is never rolled
    back, HAL result logged and ignored). In addition, the held address is recorded in
    `unconfirmedReleaseAddress` before the local removal, and only a confirmed release clears it.
  - `getLogicalAddress()`: every call reads `IHdmiCec::getLogicalAddresses()` and returns
    entry 0, logging a vector of more than one entry. A non-ok status, an empty vector, an entry
    outside `0x0..0xE` or a missing service proxy returns 0. `devType` is only logged.
  - `getPhysicalAddress()`: writes `FIXED_PHYSICAL_ADDRESS` (`0x01000000`, 1.0.0.0) without
    taking the lock or calling the HAL, and does not write through a null out-parameter. See
    [Physical address (AIDL back-end)](#physical-address-aidl-back-end).
  - `write()`: the legacy prelude and locked state guard. A frame over `AIDL_MAX_MESSAGE_LENGTH`
    (16 bytes) raises `IOException` and is not truncated. `sendMessage()` replaces `HdmiCecTx()`.
    Its `SendMessageStatus` maps onto the legacy exception set, and an undocumented value is
    logged by number and returns normally, as an unrecognised legacy status does.
  - `writeAsync()`: the legacy prelude and locked state guard, then a `LOG_EXP` line and
    `OperationNotSupportedException` instead of an asynchronous transmit.
  - `close()`: the legacy steps in the legacy order. The NULL sentinel is offered under
    `queueProducerMutex`. `IHdmiCec::close()` stands in for `HdmiCecClose()`, pending owner
    confirmation (B2). Whatever the outcome, the controller is released and the listener
    detached, and `CLOSED` is set before any raise. A successful close clears
    `unconfirmedReleaseAddress`; a failed one writes the held address to it. As on the legacy
    back-end, the local list is not cleared.
    `~DriverAidlImpl()` keeps the legacy shape and also detaches the listener on every path.
  - Receive path: `EventListener::onMessageReceived()` replaces `DriverReceiveCallback()`. It
    discards a message shorter than `MIN_RECEIVED_MESSAGE_LENGTH`, drops one that arrives after
    `detach()`, and passes a new `CECFrame` to `offerReceivedFrame()`. That method applies
    `getIncomingQueue()`'s legacy guard (an unlocked `status` read, without the unused handle
    parameter). It refuses the frame when the queue already holds `INCOMING_QUEUE_CAPACITY`
    entries (32, the legacy queue's capacity), and the callback then frees it. In the same case
    the legacy `offer()` drops the frame silently and leaks it. `onStateChanged()` and
    `onMessageSent()` only log.
- *Superseded:* this entry used to call `poll()` a byte-for-byte copy and say that all four
  copies make no HAL call. It also said that every other method keeps its DriverImpl
  counterpart's structure, guards, log lines, exceptions and statement order.
- **Include order is load bearing.**
  - The legacy `DriverImpl.cpp` includes its plain C HAL header from inside the namespace. That
    is tolerable there and must not be imitated here.
  - The generated stubs open `namespace com::rdk::hal::hdmicec` and transitively pull SDK headers
    that open `namespace android`.
  - Nesting either inside CCEC would break the link and violate the one-definition rule.
- **Global functions named like members.** `open`, `close`, `read`, `write` and `poll` collide
  with this class's member names, so they are called through the global scope operator: `::open()`
  reaches the system call and `open()` reaches the member.
- **Toolchain.**
  - C++17 is required because the generated AIDL stubs include `<optional>`.
  - This file uses the C++ libbinder backend, not the NDK backend: the pointer type is
    `android::sp<>`, the status type is `android::binder::Status`, and the server base is the
    generated `BnHdmiCecEventListener`.

### Include blocks

- **AIDL and binder headers** stay above `CCEC_BEGIN_NAMESPACE` for the include-order reason above.
- **`halcompat.h`** lives under `rdk-halif-aidl/common/current/`, not inside either frozen snapshot
  include root. A build that reaches the generated stubs but not this file is therefore missing an
  include root, not a dependency.
- **Binder kernel UAPI (`<linux/android/binder.h>`).**
  - The preflight inspects the driver node without libbinder, because the preflight's purpose is
    to decide whether touching libbinder is safe at all.
  - The header is part of the kernel UAPI and exists wherever libbinder can be built.
  - It is probed rather than required, because a sysroot can carry a prebuilt `libbinder.so`
    without the kernel headers.
  - When the header is absent, the legacy back-end is selected. That is the safe direction:
    inside libbinder, a missing or protocol-mismatched driver node aborts the process.

### cechal

- **Why an alias.** A namespace alias is used instead of `using`-declarations because the CCEC
  middleware compiles with `CCEC_NAMESPACE` undefined, so its own names sit at global scope.
- **The risk avoided.** Pulling generic generated names such as `State` in beside those names
  invites a collision that a later header could create silently.

### HEADER_OFFSET / OPCODE_OFFSET

- `DriverImpl.hpp` defines these for the legacy back-end. `DriverAidlImpl.hpp` deliberately does
  not, so they are defined here with the same values.
- The guard keeps one definition in a translation unit that has already seen the legacy header.

### AIDL_MAX_MESSAGE_LENGTH

- `IHdmiCecController.sendMessage()` states a 16-byte maximum covering the header, opcode and
  operand blocks.
- `CECFrame` carries up to `CECFrame::MAX_LENGTH` (128) bytes, and the legacy HAL specification
  documents 20. That gap makes the limit an observable difference from the legacy back-end.
- It is a named constant so that `write()` checks frames against the contract rather than a
  literal, and so the value has exactly one definition.

### MIN_RECEIVED_MESSAGE_LENGTH

The value is one byte, and it matters in both directions.

- **It cannot be zero.**
  - An out-of-process HAL can deliver an empty `std::vector<uint8_t>`. Nothing in the AIDL
    contract prevents it, and no in-process HAL could produce it, so nothing downstream was
    written to survive it.
  - The Bus reader thread hands a queued empty frame to `printFrameDetails()`. That decodes
    `Header(frame, HEADER_OFFSET)`, which reaches `frame.at(0)` and raises `std::out_of_range`.
  - Neither catch handler matches that exception: `printFrameDetails()` catches `Exception &`
    (the CCEC base), and `Bus::Reader::run()` catches `InvalidStateException &`.
  - The exception therefore escapes the thread function and terminates the process. One malformed
    message is enough.
  - Rejecting the message before any allocation is the only point inside this back-end that can
    prevent it, since `ccec/src/Bus.cpp` and the legacy back-end are outside the migration.
- **It must not be two.**
  - A one-byte frame is legitimate CEC traffic: a poll, including the header-only ping that this
    class's `poll()` transmits, carries one byte and no opcode.
  - The legacy receive path delivers such frames, and `printFrameDetails()` handles them safely:
    `Header(frame, 0)` succeeds, and the `frame.length() > OPCODE_OFFSET` test skips the opcode
    decode.
  - Requiring an opcode would drop valid frames, which would be a behaviour regression. The bound
    is the smallest decodable length, not the smallest useful one.

### HAL_LOGICAL_ADDRESS_MIN / HAL_LOGICAL_ADDRESS_MAX

- **Contract range.** `IHdmiCecController.addLogicalAddresses()` documents `0x0..0xE` for
  client-requested addresses, and `IHdmiCec.getLogicalAddresses()` returns addresses from the same
  space. 0xF is the broadcast/unregistered address. This back-end polices the HAL's side of the
  contract.
- **Why the raw value is checked.**
  - The generated proxy reads an arbitrary 32-bit integer straight out of the parcel.
  - `LogicalAddress`'s constructor narrows it, so 256 becomes 0 and 271 becomes 0xF.
  - A check applied after that conversion would accept a value the HAL never reported.
- **Why a wrong address matters.** `Connection::matchSource()` rewrites the initiator nibble of
  outbound frames with the address the middleware believes it holds, so an accepted-but-wrong
  address goes onto the CEC wire.
- **On rejection.** The method returns its existing no-address result, zero.
  `LibCCEC::getLogicalAddress()` already turns that into `InvalidStateException`, so no new
  failure path is invented.

### RECEIVE_LOG_MAX_BYTES

- The receive callback runs on a binder threadpool thread and must return promptly, so its
  diagnostic has a ceiling rather than a length that follows the message.
- A legitimate message cannot exceed the 16-byte transmit contract, while `CECFrame` can carry 128.
  The ceiling therefore shows every byte of a well-formed message and truncates a malformed one.

### RECEIVE_LOG_TEXT_SIZE

- Each byte takes two hex digits and a space, matching the `"%02X "` spelling of the legacy dump.
- `sizeof(RECEIVE_LOG_TRUNCATION_MARKER)` already includes the terminator.
- The size is derived rather than written out, so it cannot fall out of step with the ceiling.

### renderReceivedMessageHex

- **What it replaces.** A marker line per stage plus `dump_buffer()`, which issues one stdio call
  per byte (up to 128). The callback runs once per CEC message, and the HAL's own thread pool waits
  on that work.
- **Why it always renders.**
  - `dump_buffer()` could skip its work by testing `cec_log_level`, but that variable is
    file-static in `ccec/src/Util.cpp` with no accessor. No other translation unit can ask whether
    LOG_DEBUG is enabled.
  - The rendering therefore runs on every message regardless of log level, which is why it must
    be call-free and bounded.
- **How it stays cheap.**
  - Per byte: two direct nibble-to-character writes plus one separator, into a fixed stack buffer.
  - No function call, allocation or stdio inside the loop.
  - `CCEC_LOG()` cheaply discards the finished line when LOG_DEBUG is excluded.
- **Buffer type.** `text` is a reference to an array of exactly `RECEIVE_LOG_TEXT_SIZE`
  characters, so a wrong-sized buffer is a compile error rather than an overflow.
- **Worst case.** The output is at most `RECEIVE_LOG_TEXT_SIZE - 1` characters: three per
  rendered byte, at most `RECEIVE_LOG_MAX_BYTES` bytes, plus the marker.

### SLOW_HAL_CALL_WARN_MS

- **What crossing it does.** The call is not abandoned and no exception, state change or retry
  follows. The only effect is one attempted LOG_WARN line naming the operation and the elapsed
  time, so an otherwise silent stall leaves evidence a field engineer can find. Whether the line
  appears is `CCEC_LOG`'s decision, based on the configured level.
- **Where the value comes from.**
  - It is the project's only documented timing figure: the HDMI CEC HAL specification's one-second
    bound on CEC transmit completion, with 200 ms desired.
  - It is not derived from a benchmark. No performance measurement has been taken; that work is
    deferred to target hardware.
  - Every instrumented operation is a transmit or must finish well inside a transmit's budget. One
    second is crossed by no healthy HAL and by every wedged one.
- **Not a timeout.** The pinned libbinder C++ backend has no client-side transaction deadline, so
  an unbounded synchronous call stays unbounded and is only made observable. Reading this constant
  as a timeout is a serious mistake, and every synchronous HAL call in the file is annotated
  accordingly.

### HAL_CALL_CLOCK_UNREADABLE

- When a start instant carries this value, the measurement is skipped rather than computed from a
  fabricated origin.
- A diagnostic that cannot be trusted is worth less than none, and inventing an origin is how a
  bound becomes no bound.

### monotonicNowMs

- **One clock for two users.**
  - It is the single clock utility in the file, used by the synchronous-call diagnostics and by
    the context-manager probe's deadline.
  - It sits outside the `CCEC_HAVE_BINDER_UAPI` guard because the diagnostics are compiled into
    every build, while the probe also needs the binder kernel ABI.
  - One helper means there is no second clock utility to drift.
- **Monotonic, not wall clock.** A wall-clock step during initialization must not stretch or
  collapse a diagnostic measurement, or the bound that keeps `LibCCEC::init()` from stalling.
- **No overflow.** Milliseconds in `int64_t` cannot overflow on any reachable uptime, so no
  narrowing or wraparound is possible.
- **On failure** the caller must treat the measurement as unavailable rather than assume a value.

### halCallStarted

- Two clock reads per HAL operation are negligible next to an IPC round trip.
- The instrumentation can therefore be unconditional, with no sampling, counter or state of its
  own.

### warnIfHalCallSlow

- **Why measure.** An unbounded synchronous HAL call cannot be bounded, so it is measured instead.
  Below the threshold nothing is attempted, so no fast path gains a log line.
- **Why the alternatives are unavailable.**
  - The pinned libbinder C++ backend offers no client-side transaction deadline.
  - `IPCThreadState::talkWithDriver()` retries on EINTR, so neither a signal nor an interval timer
    can break a blocked transaction.
  - A bounded-join worker thread cannot cancel a transaction either, and the migration's threading
    constraint forbids one.
  - Narrowing the driver lock is forbidden, because the legacy serialization must be preserved
    exactly.
- **Effect on callers.** It never changes the caller's status translation, exception mapping or
  return value.
- **Not nonblocking.**
  - When `CCEC_LOG` emits, it formats into a stack buffer and writes through `printf`, which can
    block on the log sink.
  - That delay is never added to a HAL call, because this runs after the call returns.
- **The warning line.**
  - It is bounded against the 499-byte `CCEC_LOG` expansion limit, checked with the longest
    operation label the file passes (the `halcompat::isCompatible` one).
  - It points to the platform prerequisite rather than a finding number a log reader could not
    resolve.

### BINDER_PROBE_MAP_SIZE

- The binder driver allocates the buffer for an incoming REPLY out of the receiving process's own
  mapping, so a probe that expects a reply must map something.
- The value is far smaller than libbinder's own mapping, because the probe exchanges a single ping.
- The mapping is released before the predicate returns, on every path.

### BINDER_PROBE_MAX_ITERATIONS

- The driver interleaves bookkeeping commands with the reply: transaction-complete, no-op and
  spawn-looper. The probe must therefore drain the stream rather than read it once.
- This cap is a second, structural bound alongside the caller's timeout.

### POLL_SLICE_MAX_MS

- The probe waits in slices, and this is the slice length.
- **Why slices make the `int` safe.**
  - The caller's timeout is an `unsigned int`. A large value converted straight to the `int` that
    `poll()` takes can land negative, and `poll(..., -1)` waits without limit: the opposite of the
    bound the probe exists to provide.
  - A value clamped into `[0, POLL_SLICE_MAX_MS]` cannot do that, whatever the caller passed and
    whatever the clock reports.
- **Re-derived each iteration.** Each slice is recomputed from the absolute deadline, so no single
  accounting error can extend the total wait.

### static_assert on BINDER_PROBE_MAX_ITERATIONS

- The two bounds must not fight. If the iteration cap could expire before the deadline, it would
  cut a legitimately waiting probe short and report a usable context manager as unreachable.
- The whole slices needed to consume `DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS` must fit in
  the cap. Checking this at compile time stops a change to either constant from quietly breaking
  the relationship.
- EINTR retries and drain passes that deliver no reply also consume iterations, which is why the
  cap sits comfortably above the minimum.

### pingBinderContextManager

- **The stall it prevents.**
  - Binder handle 0 is the context manager, the `servicemanager` daemon that every name lookup
    goes through.
  - In the pinned stack, libbinder obtains it by pinging handle 0 at one-second intervals with no
    upper bound.
  - A platform whose driver works but whose `servicemanager` never started would therefore stall
    middleware initialization indefinitely.
  - This function asks the same question with a deadline, over a descriptor of its own, so no
    libbinder singleton is created and the process is left exactly as it was.
- **The exchange.**
  1. Map a small region, so the driver has somewhere to place a reply.
  2. Post one synchronous `BC_TRANSACTION` carrying `PING_TRANSACTION` at handle 0.
  3. Drain the driver's command stream under the deadline until the reply or a rejection appears.
  - A missing context manager is rejected promptly with `BR_FAILED_REPLY`, so the common negative
    case costs nothing.
- **How the wait is bounded.**
  - **One absolute deadline**, computed before the loop in 64-bit milliseconds; every iteration
    derives its budget from it.
    - The obvious alternative recomputes a residual from a fresh elapsed measurement on each pass.
    - That lets any single accounting error re-grant the full timeout on every pass, so the real
      bound becomes iterations × timeout.
  - **Sliced, clamped waits.** Each wait is clamped into `[0, POLL_SLICE_MAX_MS]` before conversion
    to `int`. This makes a negative `poll()` timeout unreachable by construction, not by the
    caller behaving reasonably.
  - **Slice versus deadline.** An expired slice is not an expired deadline: the loop continues
    while budget remains.
  - **Failed clock read.** It fails the probe as "unreachable" rather than counting as zero
    elapsed time. An unbounded wait is worse than a false negative, and a false negative only
    selects the legacy back-end.
  - **Iteration cap.** `BINDER_PROBE_MAX_ITERATIONS` still caps the loop as belt and braces, and
    the static assertion keeps it from firing before the deadline.
- **Zero timeout.** It means poll once without waiting. That meaning is load bearing, because it is
  how the negative arm is exercised.
- **Caller clamp.** The caller clamps the timeout to
  `DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS`, and the slicing makes any value that arrives
  safe regardless.
- **No `BC_FREE_BUFFER`.** The reply buffer the driver charged to the descriptor is reclaimed when
  the caller closes it.
- **Bounds and failure direction.** The wait lasts at most the timeout plus one poll slice of
  scheduling latency. Every unexpected condition counts as "unreachable", which is the direction
  that yields the legacy back-end.
- **Write-buffer layout.** The command word is immediately followed by the transaction payload,
  with no padding, because that is the layout the driver parses. Both are copied with `memcpy`
  into a byte array rather than declared as a struct the compiler would pad.

### Default probe operations

- These are the real kernel-facing calls the preflight performs in production. They exist in
  every build, with or without the binder kernel UAPI definitions.
- **Why the guard sits inside them.** The `CCEC_HAVE_BINDER_UAPI` guard is pushed down into these
  functions so that `isBinderPreflightOk()` has the same eight decision points in every
  configuration. A predicate whose branch structure changed with a build macro could not be gated
  per branch, and both arcs of each of the eight carry a record in the coverage branch manifest.
- **Without the UAPI definitions** the verdict is still false, because the version read fails, and
  the reason is logged rather than inferred.
- **`::open` and `::close`** are qualified for consistency with the rest of the file, where
  qualification is required because `DriverAidlImpl` has members of both names.

### defaultOpenBinderNode

- `::open` is variadic, so its address has a type that no fixed-arity function pointer can hold.
  That is why a thin wrapper is used instead of `&::open`.

### copyNodeIdentity

- This is the one place the POSIX stat types are converted, so no host's `dev_t` or `ino_t` width
  leaks across the probe seam.
- Each field widens into the widest plain integer that can hold it, so nothing is truncated on any
  supported host.

### defaultIdentifyBinderDescriptor

- **Descriptor, not path.** The identity is taken from the descriptor, so it describes the object
  this process already holds open. Nothing outside the process can change it afterwards, which
  makes it usable as the reference that the later path re-check compares against.
- **`errno`.** On a `fstat` failure, `errno` carries the reason. For a null `identity`, it is set
  to EINVAL.

### defaultIdentifyBinderPath

- **Why `stat`, not `lstat`.** This is required, not an oversight. A binderfs deployment may
  publish `/dev/binder` as a symlink to `/dev/binderfs/binder`, and the identity must be that of
  the node libbinder will actually open. Refusing to follow the link would reject a real,
  correctly provisioned platform.
- **`errno`.** It carries the resolution failure, or EINVAL for a null argument.

### defaultReadBinderProtocolVersion

- Failure is reported the way the preflight expects, rather than letting a kernel type or an ioctl
  return code cross the seam.
- **`errno`.** It is the ioctl's reason, or ENOSYS when the build has no binder kernel ABI. That
  keeps the caller's log line meaningful either way.

### defaultPingBinderContextManager

- Without the binder kernel ABI this path is unreachable in practice, because the version read has
  already failed the predicate.
- It still reports "unreachable", the direction that yields the legacy back-end.

### defaultCloseBinderNode

- The preflight ignores the return value, exactly as the code it replaced ignored `::close`'s.
- **Precondition:** the descriptor came from `defaultOpenBinderNode()`.

### DriverAidlImpl::EventListener

- **What it replaces.** It is the AIDL counterpart of the legacy
  `DriverImpl::DriverReceiveCallback()` and `DriverImpl::DriverTransmitCallback()` function
  pointers.
- **Where it is defined.** In the implementation file, so the generated server-side base is not
  pulled into every translation unit that includes `DriverAidlImpl.hpp`.
- **Three callbacks.** Only three are implemented, because `BnHdmiCecEventListener` already
  supplies `onTransact()` and concrete `getInterfaceVersion()`/`getInterfaceHash()`.
- **Behaviour versus diagnostics.**
  - Only `onMessageReceived()` carries behaviour; the other two are diagnostics.
  - Acting on them would be new behaviour with no legacy counterpart, since an in-process HAL can
    neither change state behind the middleware's back nor vanish.
- **Always `ok()`.**
  - A `oneway` callback has no caller to receive a fault.
  - An exception escaping into `onTransact()` would be worse than dropping one frame.
  - Dropping is the disposition the legacy receive callback takes when it deletes the frame on
    throw.
- **Lifetime: why detachment, not destruction.**
  - **Two references.** The owner holds one `android::sp<EventListener>`. The HAL holds its own
    strong reference, taken when the listener crossed the binder boundary in `IHdmiCec::open()`.
  - **The hazard.** Releasing the owner's reference may not destroy the object. The HAL's
    reference can keep it alive, and with it a back pointer to a `DriverAidlImpl` that is being,
    or has been, destroyed.
  - **Why it is reachable.**
    - The AIDL "no further callbacks" guarantee holds only after a successful
      `IHdmiCec::close()`.
    - A failed close still drives this back-end to CLOSED.
    - `~DriverAidlImpl()` swallows that failure, as `~DriverImpl()` does.
  - **The fix.**
    - Every owner-touching callback runs under the listener's lock.
    - `detach()` nulls the back pointer under that same lock, so a detached listener logs and
      drops instead of dereferencing freed state.
    - The owner detaches on both arms of `close()`, on the failure arms of `open()`, and
      unconditionally in its destructor.
- **Lock order.** The listener lock is a leaf, with one exception: a receive callback holds it
  while calling `EventQueue::offer()`, which takes and releases the queue's own lock.
  - **No inversion.**
    - Teardown never runs in the opposite order: `close()` calls `rQueue.offer(0)` first, then
      `detach()`.
    - The two acquisitions never nest in both directions, so there is nothing to deadlock on.
  - **No capacity wait.**
    - A full queue makes `offer()` discard rather than block.
    - `EventQueue::poll()` waits on its condition variable outside the queue lock, so a blocked
      consumer cannot hold the lock against a producer.
  - **What `detach()` does wait for.**
    - `offer()` acquires the queue lock, may allocate as it appends, and locks and broadcasts the
      queue's condition variable.
    - `detach()` waits for an in-flight callback to finish that work: a completion bound, not a
      proven duration.
  - **The owner's instance lock.** The listener lock is never held while the instance lock is
    required the other way round: a callback reaches the owner only through `getIncomingQueue()`,
    which takes no instance lock.
- **No `getInstance()` downcast.**
  - The legacy receive callback resolves its target with
    `static_cast<DriverImpl &>(Driver::getInstance())`.
  - That is well defined only because it is registered from `DriverImpl::open()`, so it never runs
    on the AIDL path.
  - Reusing that route here would be an ill-typed downcast and undefined behaviour.
- **Not copyable.** `RefBase`, reached through `BnHdmiCecEventListener`, keeps its copy
  constructor and copy assignment private.

### DriverAidlImpl::EventListener::EventListener

- The owner is stored as a pointer, not a reference, so that `detach()` can null it.
- A reference member could not be cleared. The only way to stop the callbacks would then be to
  destroy an object the HAL may still hold a strong reference to.

### DriverAidlImpl::EventListener::detach

- **What it guarantees.**
  - This is the whole use-after-free (CWE-416) guarantee, resting on one property: every callback
    that touches the owner holds the listener lock for its entire body.
  - Taking that lock here means `detach()` cannot return while a callback is part-way through
    dereferencing the owner.
  - That blocking property is why it takes the lock rather than simply writing a null.
- **Called on failure paths too.** It runs on every path that ends a session, including failed
  ones, because a failed `IHdmiCec::close()` leaves the HAL entitled to keep calling.
- **Idempotent by design.** `close()` and `~DriverAidlImpl()` may both reach it for the same
  listener. `open()`'s failure arm may reach it for a listener that never received anything.
- **What it waits for.**
  - A callback cannot wait for queue capacity, since a full queue makes `EventQueue::offer()`
    discard.
  - It can acquire the queue's lock, allocate as the offer appends, and lock and broadcast the
    queue's condition variable.
  - `detach()` is bounded by that work finishing, and by nothing shorter.
- **Detachment is not destruction.** The HAL holds its own strong reference, so destruction is not
  the owner's to schedule.

### DriverAidlImpl::EventListener::onMessageReceived

- **What it replaces.** The legacy `HdmiCecRxCallback_t`, step for step equivalent to
  `DriverImpl::DriverReceiveCallback()`: copy the payload into a new `CECFrame`, log it, and hand
  it to the incoming queue.
- **Goes through the owner, not the queue member.** This is the load-bearing part.
  - The handoff reaches the queue through `getIncomingQueue()`, which raises when the driver is not
    OPENED.
  - That raise rejects a callback arriving during or after a close and drives the release of the
    frame.
  - Offering to the member directly would accept frames the legacy path rejects.
- **Ownership is explicit, because the queue can refuse.**
  - `EventQueue::offer()` returns void and silently discards its argument at capacity. Offering and
    then dropping the pointer would leak one heap frame per event, without bound, whenever the Bus
    reader falls behind.
  - `DriverAidlImpl::offerReceivedFrame()` therefore reports whether it took the frame:
    - **On true**, the local pointer is cleared at once, so neither later code nor any catch arm can
      touch a frame the queue owns. Nothing between the handoff and that assignment can throw.
    - **On false**, this callback still owns the frame, releases it, and says so at LOG_EXP.
- **Receive limit.**
  - The refusal point is the queue's capacity, the same 32 entries the legacy queue gives received
    frames.
  - The refusal log line names that capacity and says the frame was released.
- **One bounded diagnostic line.**
  - The success path makes one `CCEC_LOG()` call, with the byte count and a hex rendering of at
    most `RECEIVE_LOG_MAX_BYTES` bytes.
  - The alternative, a marker per stage plus `dump_buffer()`, would put unbounded per-byte logging
    on a binder thread for every message. Each marker costs a `vsnprintf`, a timestamp and a
    `printf`.
  - The rendering keeps the `%02X ` spelling.
  - A refusal, state-guard rejection or length rejection is logged at LOG_EXP. A debug line with no
    LOG_EXP line after it therefore means the frame was queued.
- **No IPC, but not wait-free.**
  - The body is copy-and-enqueue, with no HAL call and no IPC.
  - The listener lock, the queue lock, the frame allocation and the condition-variable broadcast
    can each delay the thread, and no time bound is claimed.
  - The only change to the receive path is which thread produces into the queue the Bus reader
    already drains.
- **Order of checks.**
  1. **Length check first.** It needs neither the owner nor the lock, so an under-length message
     never contends for the listener lock.
  2. **Detach check second.** The back pointer is read under the lock. Null means the session has
     ended, and the message is logged and dropped, not allocated and not delivered.
  - The detach arm is reachable after a failed close, not merely defensive.
  - The lock is held across the whole delivery.
- **The six exits.** In each, the frame is never leaked and never double-freed:
  1. queued;
  2. rejected for length before allocation;
  3. dropped after detach before allocation;
  4. refused at the receive limit;
  5. rejected by the OPENED-state guard;
  6. lost to an allocation or append failure.
- **What is validated, and what is not.**
  - A queued frame carried at least `MIN_RECEIVED_MESSAGE_LENGTH` bytes and fitted
    `CECFrame::MAX_LENGTH`. `CECFrame::append()` raises past the maximum, and the general catch
    arm releases the frame.
  - The message is not decoded: no header, opcode or operand is examined, and no claim is made that
    it is well formed.
- **Refusal means the reader is stalled.** It means the Bus reader is not draining. Dropping is
  the only choice, since the alternative is unbounded growth on a path a remote HAL drives, and the
  drop is logged.
- **Closed-state rejection.**
  - It has its own catch arm, so it is distinguishable in a log from an allocation or append
    failure.
  - It is the AIDL counterpart of the legacy delete-on-throw cleanup.
  - The guard raises before anything is offered, so the frame is still the callback's to release.

### DriverAidlImpl::EventListener::onStateChanged

- **Deliberately no action.** Offering the NULL sentinel on a transition to `State::CLOSED`, so a
  blocked Bus reader unwinds, was considered and rejected. It has no legacy counterpart, because an
  in-process HAL cannot close underneath the middleware, and new behaviour is out of bounds for a
  transport migration. Death recipients and `linkToDeath()` are omitted for the same reason.
- **Why the whole body is in a catch-all.**
  - An exception leaving a `oneway` callback unwinds into the generated `onTransact()`, which does
    not expect one, and then into libbinder's threadpool loop. That would take down a binder thread
    over a log line.
  - The thing that can raise is `cechal::toString()`: it returns a `std::string` by value for each
    argument, so either construction can throw `std::bad_alloc`.
  - On a receive path a HAL can drive as fast as it likes, that is a real possibility.
  - Dropping the diagnostic is the only disposition, since there is no caller to report to.
- **Allocation-free fallback.**
  - It renders both states as plain integers through `%d`, and constructs no `std::string`.
  - A handler that allocated would defeat the containment, having most likely been reached because
    an allocation failed.
  - `CCEC_LOG()` is safe here: it formats into a fixed 500-byte stack buffer with `vsnprintf` and
    allocates nothing (`ccec/src/Util.cpp`).

### DriverAidlImpl::EventListener::onMessageSent

- **Diagnostic only.**
  - It mirrors `DriverImpl::DriverTransmitCallback()`, which the legacy back-end also uses only
    for logging.
  - Synchronous transmit results reach callers as the return value of
    `IHdmiCecController::sendMessage()`, so nothing here feeds back into the middleware.
- **Logs every status.**
  - The legacy callback logs only on failure.
  - Here, `SendMessageStatus` has no back-end-independent success value: `ACK_STATE_0` means
    acknowledged for a directed message and rejected for a broadcast.
  - Deciding "failure" would mean re-deriving the destination nibble, duplicating `write()`'s
    translation where it could drift. So the status is reported verbatim.
- **Includes the message bytes.**
  - A status alone cannot be tied to a transmit when several are in flight, and a length cannot
    distinguish two same-sized frames.
  - The bytes are rendered as lowercase hex into a stack buffer sized at compile time from
    `CECFrame::MAX_LENGTH`, so nothing the HAL sends can make it grow.
  - A longer message is truncated in the rendering only, with an ellipsis. The HAL cannot
    legitimately echo one, since `write()` refuses to send it.
- **Same containment as `onStateChanged()`.**
  - The hex rendering allocates nothing, but `cechal::toString()` can raise `std::bad_alloc`, and
    the callback fires once per transmit.
  - The fallback reports the status as a plain integer and the length, without repeating the
    construction that failed.

### DriverAidlImpl::EventListener::ownerMutex

- **Makes `detach()` synchronous.** It is held for the whole of every callback that dereferences
  `owner`, and by `detach()` while it nulls it. Together, those two facts make `detach()`
  synchronous with respect to in-flight delivery rather than merely eventual.
- **Separate from the owner's instance lock.** A callback must be able to complete while the
  owner's teardown holds that instance lock, which is exactly what `close()` and
  `~DriverAidlImpl()` do.

### DriverAidlImpl::EventListener::owner

- It is a pointer precisely so it can be nulled.
- Null means the owner has detached, and this listener, which the HAL may still reference, must not
  touch it again.

### DriverAidlImpl::DriverAidlImpl

- **Set by the initializer list:** the lifecycle state, the retained legacy handle field, the
  queue's capacity, and the null availability reason.
- **Default-constructed:** the lock, the local address list and the two session proxies. For the
  proxies, that means null.

### DriverAidlImpl::~DriverAidlImpl

- **The three paths that reach the detach**, and the reason it sits outside the state test:
  1. `close()` threw, and its exception was just swallowed;
  2. `close()` returned silently, or succeeded and already detached, making this the idempotent
     second call;
  3. the instance was never opened, but `open()` built a listener before failing.
- **Why it matters.** Leaving any of these with an attached listener is a use-after-free
  (CWE-416). This object's storage is about to end, while the HAL's strong reference can keep the
  listener callable.

### DriverAidlImpl::close

- **Six observable steps, in legacy order:**
  1. state test;
  2. move to CLOSING;
  3. sentinel offer;
  4. HAL close;
  5. listener detach and release;
  6. move to CLOSED, before any raise.
- **Sentinel before the HAL transaction.** This is the legacy order, and it is relied upon.
- **A failed close still closes this side.** No controller is held, no listener is attached, and
  the state is CLOSED before the exception is raised. B2 concerns only which HAL call step 4 makes.
- **A failed close records the held address.** The HAL removed nothing, so the local entry, if
  any, is written to `unconfirmedReleaseAddress` before the raise; the next registration releases
  it first.
- **Sentinel under `queueProducerMutex`.**
  - `close()` is a producer on the queue, not merely its terminator.
  - The receive path establishes that there is room before it parts with a frame. That occupancy
    check is only stable while every producer that can raise occupancy is serialized against it.
  - Offering without the lock could land the sentinel between the receive path's check and its
    offer. `EventQueue::offer()` would then silently discard the frame while the receive path
    reported it accepted, leaking it.
- **A full queue drops the sentinel.** The sentinel shares the queue's 32 slots with received
  frames, as on the legacy path, so a close against a full queue loses it. No reader is blocked
  in `EventQueue::poll()` then, and the reader's next read() raises at its entry guard.
- **Lock nesting.**
  - The lock is held for one offer, with no IPC, inside the instance lock. That is the only
    nesting order that exists.
  - `offerReceivedFrame()` takes `queueProducerMutex` alone, never the instance lock, so no
    inversion is possible.
  - The offer statement itself matches the legacy line.
- **Detach on both arms.**
  - It runs before the failure check. A close that reported failure leaves the HAL entitled to
    keep calling, and the failure arm exits through an exception that `~DriverAidlImpl()`
    swallows.
  - Detaching only on success would leave that path holding a listener with a live back pointer
    into an object about to be destroyed.
- **Detach after the sentinel.** It runs after the sentinel offer, so the queue lock is never held
  while the listener lock is awaited.

## ccec/src/DriverAidlImpl.cpp (part 2 of 2)

Detail moved out of the condensed comments from `read()` to the end of the file. Each entry
holds what the source comment no longer carries; the source keeps the contract in brief.

- **Superseded statements.** Two pre-refine claims in this part are no longer current and
  are kept here only as history: (1) `write()` and `offerReceivedFrame()` said the plan
  enumerates "exactly three" authorized observable differences and "closes the list", and
  `read()` spoke of "the two registered observable differences, on getLogicalAddress() and
  write()"; enabling the driver now also discovers one DeviceType-derived logical address and
  registers it with `addLogicalAddresses`, an additional authorized difference, so the list is
  not three. (2) `isValidLogicalAddress()` said that copying it verbatim keeps the two back-ends
  "indistinguishable to Connection"; the walk is still a verbatim copy, but after the
  enable-time registration the AIDL back-end's local list can hold an address the legacy list
  does not.

### CCEC::DriverAidlImpl::read

- Apart from the class name it is a copy of `DriverImpl::read()`: the same entry guard, the
  same re-check under the lock when the queue yields nothing, and the same flush-then-raise on
  the NULL close sentinel. Copying rather than re-deriving is the specification, because any
  other divergence would be a divergence in the `Bus` reader's behaviour.
- **The flush loop is the legacy one, defect included.** The plan specifies the method as
  byte-identical to `DriverImpl::read()`, and it is. The flush dequeues into `inFrame` and
  evaluates `frame = *inFrame` with no null test, while the only entries that reach it are
  received frames and `close()`'s NULL sentinels. `close()` offers a sentinel on each transition
  out of OPENED, so a stop/reopen/close, or a close racing a second close, can leave two: the
  checked poll at the top of the loop consumes the first and the flush meets the second and
  dereferences NULL on the `Bus` reader thread.
- **Why it is not fixed here.** The defect is `DriverImpl::read()`'s, and that file is
  reference-only in this migration. Fixing it in one back-end alone would be an unauthorized
  difference between the two. The Project Guide records it under the repeated in-process
  restart risk, whose repair adds the null check to the flush loops of both `DriverImpl::read()`
  and `DriverAidlImpl::read()` in a separately scoped change.
- No test drives a second sentinel into the flush: on either back-end the case would be a null
  dereference, which crashes the test runner rather than failing an assertion.
- Superseded: an earlier revision null-checked every entry in this flush, and an earlier queue
  design gave `close()`'s sentinel a reserved slot; both departures from the legacy body were
  withdrawn.
- `close()` is the producer of the NULL sentinels this method drains.

### CCEC::DriverAidlImpl::writeAsync

- The two prelude statements sit outside the lock, in that order, because that is where
  `DriverImpl::writeAsync()` has them and the order is observable: an empty frame raises
  `std::out_of_range` out of `printFrameDetails()`'s header decode before the state guard is
  reached. Reordering the prelude for tidiness would be an unregistered behaviour change.
- `write()` is the synchronous path every production caller reaches.

### CCEC::DriverAidlImpl::write

- Status translation, each arm reproducing one arm of the legacy mapping:
  - a non-ok binder status (transport failure, `EX_ILLEGAL_STATE`, dead binder) raises
    `IOException`, as `err != HDMI_CEC_IO_SUCCESS` does;
  - `BUSY` (arbitration failed after two attempts, nothing sent) raises `IOException`, as the
    legacy send-failed family does;
  - a directed message with `ACK_STATE_1` was not acknowledged and raises `CECNoAckException`;
  - a broadcast with `ACK_STATE_0` raises `CECNoAckException` only on the CEC CTS 9-3-3 arm (a
    rejected `REPORT_PHYSICAL_ADDRESS`) so the caller retries, and returns normally for every
    other opcode, as the legacy implementation does;
  - a directed `ACK_STATE_0` and a broadcast `ACK_STATE_1` both mean success;
  - any other value returns normally, as an unrecognised status does on the legacy back-end.
- The destination nibble is read from `frame.at(0) & 0x0F`, the legacy expression, inside the
  arms that need it rather than before the switch, so `BUSY` is decided without touching the
  frame.
- **The default arm.** `sendMessage()`'s result is an `int32_t` the generated proxy reads out
  of the parcel, so an out-of-process HAL can return any 32-bit value. A value outside the
  three documented enumerators is logged by number at `LOG_EXP`, and the method then returns
  through the same "Send Completed" diagnostic as every successful arm. `DriverImpl::write()`
  tests the HAL result against a closed set of five failure values and the not-acknowledged
  arms and takes no action on anything else, so both back-ends give the caller the same
  outcome for an unrecognised status.
- **Superseded.** An earlier revision raised `IOException` from this arm and registered it
  for the specification owner as an observable difference, on the grounds that a suppressed
  transmit reported as completed is never retried. Review required the legacy fall-through,
  because the raise was not on the authorized list of differences; the numeric log line keeps
  the HAL's answer diagnosable.
- Only the numeric status is logged: the value is HAL-controlled and must never reach the log
  as text or as a format string, the same discipline the address validation follows.

### CCEC::DriverAidlImpl::removeLogicalAddress

- The legacy shape is reproduced rather than improved: raising where the legacy back-end
  returns silently would be an unregistered behaviour change.
- Whether `source` is the held address (the local entry or `unconfirmedReleaseAddress`) is read
  before the local removal, and a held address is written to `unconfirmedReleaseAddress` at once,
  before the list removal and the HAL call. An ok `true` release of the recorded address clears
  the record. A false or non-ok release keeps it and is not raised; an exception from the request
  allocation or the proxy keeps it and propagates. Nothing else changes.

### CCEC::DriverAidlImpl::isValidLogicalAddress

- Copied rather than re-derived because there is nothing in it for the transport to change.
  `Connection` is its only production caller.

### CCEC::DriverAidlImpl::poll

- Consulting the HAL's `getState()` instead would create a second source of truth beside the
  middleware's own state machine.
- The trailing `#if 0` block is carried verbatim so the method stays a character-for-character
  copy of its counterpart and the two files diff cleanly; it is not new dead code and not a
  placeholder.

### CCEC::DriverAidlImpl::getIncomingQueue

- The guard, not the return, is the load-bearing part of the accessor.
- The unlocked read of the plain-`int` `status` is a data race that exists today between the
  vendor HAL's thread and a closing thread; here a binder thread takes the HAL thread's place.
  Adding a lock or an atomic would be an unregistered behaviour change.

### CCEC::DriverAidlImpl::offerReceivedFrame

- **Why the room check holds.** While `queueProducerMutex` is held nothing can add an entry;
  consumers ignore the lock but only remove, which lowers occupancy. Removal-only consumers are
  not enough on their own: `close()` is a producer, and a close exempt from the lock could land
  its sentinel between the check and the offer, after which `EventQueue::offer()` would discard
  the received frame silently while this method reported it accepted, leaking one frame per
  event. `close()` therefore takes the same lock for its sentinel offer.
- `osal/include/osal/EventQueue.hpp` is outside this migration, so its silent discard at
  capacity is worked around here rather than fixed there.
- **Receive depth and the sentinel are at parity.** The legacy queue keeps
  `CCEC_OSAL::EventQueue`'s default (`EventQueue(size_t cap = 32)`) and `DriverImpl`'s receive
  callback offers straight onto it, so received frames fill 32 slots and `close()`'s sentinel
  shares them. This queue is built with `INCOMING_QUEUE_CAPACITY` (32) and refuses only when
  full, so received frames fill the same 32 slots, the same frame is accepted at every
  occupancy, and a sentinel offered to a full queue is dropped on both back-ends.
- **The one deliberate difference from the legacy callback is ownership, not capacity.** Where
  `DriverImpl::DriverReceiveCallback()` hands a frame to a full queue and loses it,
  `EventQueue::offer()` returning void, this method refuses it and the listener releases it.
  Which frames reach the reader is unchanged.
- **Acceptance is reported, not read back.** Producers are serialized, so the queue cannot fill
  between the check and the offer, and an offer after a passing check always lands. Reading the
  occupancy back could not tell a frame that took the 32nd slot from one the queue discarded,
  because both leave it full.

### CCEC::DriverAidlImpl::printFrameDetails

- The catch naming `Exception`, the CCEC base, is what lets an empty frame's
  `std::out_of_range` leave the method, which is how `write()` and `writeAsync()` raise out of
  their prelude ahead of their state guard on both back-ends alike.
- The bare `\n` terminator, where every other log line in the file uses `\r\n`, is how the
  legacy line reads; normalizing it would break the character-for-character copy.

### CCEC::DriverAidlImpl::isBinderPreflightOk

- **The eight decision points, in order:** the path is empty; the node could not be opened;
  the object behind the descriptor could not be identified; it is not a character device; it
  is not owned by root; its version could not be read; the version differs from this build's;
  the context manager did not answer.
- The `CCEC_HAVE_BINDER_UAPI` guard lives inside the default probe rather than around this
  body, so a build without the binder kernel ABI definitions still runs every arm and still
  reaches false, by failing the version read with `ENOSYS` and logging why.
- The abort hazard is observed, not inferred: on a driverless host merely reaching
  `ProcessState::self()` raises SIGABRT. A missing context manager is the other hazard; it
  blocks rather than aborting, which is what the bounded handle-0 check answers. See also
  `expectedBinderProtocolVersion()`.
- The custody slot is cleared before any arm can return, so every later arm may return early
  without touching it.
- **Decision point 2.** `errno` is captured first because every call between the failed open
  and the report, `CCEC_LOG` included, may overwrite it. `ENOENT` means the kernel carries no
  binder driver: the ordinary, healthy state of a legacy-only SOC and the case the fallback
  exists for. Any other errno means the node is there and unusable (`EACCES` for permission or
  policy, `EMFILE`/`ENFILE` for exhausted descriptors, `ENODEV` for an unregistered driver),
  which an integrator must act on. Collapsing them would make a broken platform
  indistinguishable from a legacy-only one; the second arm logs the errno text as well as the
  number.
- **Decision point 3.** Asking the descriptor rather than the path describes what the process
  already holds open, the only form of the question whose answer cannot change underneath it,
  and it is the reference the caller's re-check compares a fresh resolution against.
- **Decision point 4.** Every supported layout publishes a character device: a devtmpfs node
  under `/dev`, or a binderfs node reached directly or through a symlink. A regular file at the
  path is the shape a substitution takes when an unprivileged process wins a race to create it.
- **Decision point 5.** A node owned by anyone but root is one an unprivileged process could
  create or replace, and selecting the AIDL path against it would put the whole HAL boundary,
  every frame sent and delivered, behind whatever registered itself there. Only the numeric
  owner is logged; nothing HAL- or filesystem-controlled is rendered as text.
- **The writable-beyond-owner observation.** It cannot change the verdict and nothing reads
  it; it sits after the type and owner checks so it describes a root-owned character device. A
  node writable beyond its owner can be opened by every process in that group or on the box;
  on a platform that also fails to authorize service registration it is the precondition of
  the HAL impersonation recorded on `isServiceAvailable()`, and this line is the only place an
  integrator learns the two coexist. **It must stay a log line, not a refusal:** a binder node
  has to be openable by every client process, so AOSP-derived platforms (this port vendors
  `android-13.0.0_r74`) publish it broadly accessible by design and binderfs assigns its own
  mode. Refusing would decline the AIDL path on conformant production platforms while passing
  in CI, whose guest node is root-owned with everything running as root. What carries a verdict
  is the two checks before it and the five-attribute identity comparison immediately before the
  lookup, which catches the mode changing inside the check-to-use window. The mode is printed in
  octal, at `LOG_INFO`: the standard node is `0666`, so the line describes every healthy start
  and is information, not a fault; the platform faults the decision points report stay at
  `LOG_WARN`.
- **Decision point 6.** Logged at `LOG_WARN`: a missing node is a legacy-only SOC behaving as
  designed and alone stays at `LOG_INFO`, whereas here the node existed and opened, then refused
  the one ioctl every binder client issues first. No correctly provisioned platform does that;
  it is a mis-created device node, a driver that is not binder, or a kernel and libbinder that
  disagree about the interface. It sits at the level of its siblings, the failed open and the
  protocol mismatch, because all three describe a platform fault rather than an absent HAL.
- **Decision point 7.** libbinder enforces an exact match when it opens the driver, so a
  difference in either direction fails every open.
- **The clamp.** The timeout parameter is unsigned, so a caller can name a value far beyond any
  plausible `servicemanager` start-up delay, every millisecond of it spent blocking
  `LibCCEC::init()`. Clamping keeps the ceiling in one place and the probe's own slicing bounds
  each individual wait. Zero means "do not wait at all", which is how the negative arm is
  exercised.
- **Custody.** A caller that asks keeps the validated descriptor and its identity, so the node
  it re-resolves before use can be compared with the node actually validated, and holding the
  descriptor stops the inode being recycled under that comparison. A caller that asks nothing
  gets the behaviour the predicate had before custody existed.

### CCEC::(anonymous namespace)::REASON_TRANSPORT_UNAVAILABLE, REASON_NO_COMPATIBLE_SERVICE, REASON_QUERY_FAILED

- Declared beside `isServiceAvailable()`, their only assigner, rather than at the top of the
  file; the selection helper in `ccec/src/Driver.cpp` is their only printer. Keeping them beside
  their sole producer stops an arm being added without a phrase, or a phrase being reworded away
  from its arm.
- The first two are the exact texts the selection helper emits. `tests/L1Tests/ccec/test_DriverAidl.cpp`
  matches the first ("binder transport is unavailable"), and the
  `tests/L2Tests/ccec/test_DualPathIntegration.cpp` section of these notes quotes both word for
  word. Neither contains the selected-path literal, which keeps a grep for that literal at exactly
  one hit per process.
- `REASON_QUERY_FAILED` covers the catch-all: a query that neither succeeded nor cleanly
  declined is a different platform condition from an absent transport or an unusable service.

### CCEC::(anonymous namespace)::NODE_IDENTITY_DIVERGED_DEVICE … NODE_IDENTITY_DIVERGED_UID

- One bit per `BinderNodeIdentity` attribute lets the re-verification diagnostic say which
  attribute changed as a single number rather than a sentence assembled at runtime.
- They are plain integers and not derived from the struct layout, because a bit whose meaning
  depended on member order would silently re-point when a member moved.

### CCEC::(anonymous namespace)::RetainedBinderNode

- **Why it exists.** The preflight hands its validated descriptor to `isServiceAvailable()`,
  which has seven exits (four declines, one success, one incompatible-service arm, one
  catch-all), and the descriptor is a `/dev/binder` open: a live driver context nothing reclaims
  until the process exits. Releasing it by hand on seven paths is accounting that survives review
  and then fails on the eighth path someone adds. Scope-bound release makes it structural,
  including an exception unwinding through the holder, which no hand-written release could cover
  because the catch-all sits outside it.
- It is not an abstraction of the prohibited kind: no virtual dispatch, no allocation, no
  ownership question and no indirection at the HAL boundary. It is the `AutoLock` idiom applied
  to a descriptor, file-local to this translation unit.
- The copy operations are declared private and never defined. The probe is bound to one that
  outlives the holder (a caller's argument or `defaultBinderProbe()`'s static), which the single
  call site provides.
- The holder becomes non-empty only when the preflight writes into `descriptorSlot()`; after the
  call it is non-empty if and only if custody was transferred. The destructor makes the custody
  window closed by construction rather than by care.

### CCEC::(anonymous namespace)::binderNodeIdentitiesMatch

- The question is "has the node changed", not "is it still acceptable"; only the first closes a
  check-to-use window.
- `device` and `inode` identify one filesystem object and `rdev` is the driver's major/minor
  pair, so a replacement node (newly created, bind-mounted over, or reached through a re-pointed
  symlink) differs in at least one of them.
- `mode` and `uid` catch the other half of the window. The preflight checks ownership and file
  type once, on the object it holds open; libbinder then resolves the same pathname itself, and
  every client opens that node too. An attacker who cannot replace the node can still `chmod` or
  `chown` the same inode inside the window, and `(device, inode, rdev)` alone would report that
  as unchanged; the preflight's own checks ran before the change. Comparing all five makes the
  validation a statement about the node at the moment of use.
- It is safe at any platform baseline (an earlier version of the comment argued otherwise and
  was wrong): comparing `mode` requires only that it did not change, so a restrictive (0600) and
  a broadly accessible (0666, which AOSP-derived layouts publish deliberately) node both pass.
  Refusing is nonfatal: the AIDL path is declined and the legacy back-end is selected.
- `observed` comes from re-resolving the pathname or from identifying a freshly opened
  descriptor. Which attributes diverged is reported in the log, not the return value; the
  function changes no state and decides nothing about the back-end, and both call sites turn
  false into the same nonfatal decline. It logs numbers only, matching the address-range and
  transmit-status diagnostics. See also `DriverAidlImpl::BinderNodeIdentity` and
  `DriverAidlImpl::isServiceAvailable()`.
- The diagnostic is emitted here rather than at the call sites so neither grows a conditional:
  the second call site is a disjunction whose other operand is a failed `fstat`, and splitting it
  to report a divergence would turn one documented decision into two. The mask and both tuples
  let a reader tell a substituted node from a re-permissioned or re-owned one without a second
  run; everything but the modes is decimal and unsigned.

### CCEC::(anonymous namespace)::reverifyBinderNodeBeforeLookup

- It is the last step between the check and libbinder's own use; it narrows that window but
  cannot close it.
- **Identity.** libbinder resolves the same pathname independently and, on the pinned stack,
  aborts rather than returns if it finds no usable binder driver, so the name is resolved again
  and compared with the validated identity. A mismatch means the window was lost, and the AIDL
  path is declined nonfatally, which is the whole value of declining. All five attributes are
  compared, catching both a substituted node and the same object re-permissioned or re-owned.
- **Liveness under a bound.** `defaultServiceManager()` polls until binder handle 0 resolves,
  with no upper bound, so a wedged or dying `servicemanager` is the dominant stall for a
  correctly provisioned driver. Asking again here under the preflight's bound turns a manager
  that died or wedged between the preflight and this ping from an unbounded hang into a decline.
  One that fails after this ping is still waited on without bound, as is everything inside
  `getService` once entered: the pinned libbinder has no client-side deadline. That residual
  window is recorded, with its owners, on `isServiceAvailable()`.
- **Why a second descriptor.** The ping maps the driver's transaction buffer, and the binder
  driver permits exactly one mapping per open descriptor for its lifetime: unmapping releases the
  address range but not the right. A re-ping over the retained descriptor would fail on every
  correctly provisioned platform, a check that always says no. The fresh descriptor is
  identified with `fstat` and compared with the same validated identity before use, and the
  retained descriptor stays held throughout, which keeps the inode from being recycled under
  either comparison. It is opened with the preflight's flags, so the two are equivalent in every
  respect that matters.
- The timeout is the value the preflight used, clamped there and again here so initialization
  stays bounded whichever route reached the ping. A false return covers: the path no longer
  resolves or resolves elsewhere; the fresh descriptor is not the validated node; it could not be
  opened or identified; or no context manager answered. The precondition guarantees `validated`
  describes an object that still exists. See also `DriverAidlImpl::isBinderPreflightOk()` and
  `DriverAidlImpl::isServiceAvailable()`.

### CCEC::(anonymous namespace)::sanitizedInterfaceHash

- The hash arrives from another process, so neither its content nor its length is under this
  process's control. A frozen AIDL hash is a short hexadecimal digest, but a broken or hostile
  server can send control characters that corrupt a terminal or log parser, or a payload long
  enough to bury the rest of the diagnosis.
- Replacing bytes with `?` keeps the length and shape visible; the true length is reported on
  truncation because truncating silently would be its own small lie; `<empty>` stops an empty
  hash reading as a formatting fault. The argument is not modified.

### CCEC::DriverAidlImpl::describeObservedInterfaceHash

- The phrases let a reader see whether the server answered with a frozen digest, the failure
  marker or an unfrozen development marker, without the code claiming which of halcompat's rules
  did the rejecting.
- halcompat's public `isCompatible<I>()` and `atLeast<I>()` apply the empty, `"-1"` and
  `"notfrozen"` gates internally and return only a verdict, and its `namespace detail`
  (`rdk-halif-aidl/common/current/halcompat.h:80`) holds nothing that takes a hash, so the markers
  are read from halcompat's own gate order rather than invented.

### CCEC::DriverAidlImpl::observedMetadataWouldBeAccepted

- `halcompat.h:26-27` states that client code never calls `getInterfaceVersion()`/
  `getInterfaceHash()` directly, and its `namespace detail` at `:80`, opened by the "Internal
  encoding machinery" note, holds the version predicate called here. This is the one place in
  the file that departs from them.
- **The public surface.** halcompat's public API is three templates: `getService<I>()` (`:143`),
  `isCompatible<I>(service, allowUnfrozen)` (`:158`) and
  `atLeast<I>(service, era, major, minor, bugfix, allowUnfrozen)` (`:181`). All take a service
  proxy; there is no public predicate over an already-observed version, no accessor for an
  observed hash or version, and nothing for a hash alone. The version half therefore has only the
  internal predicate, and the hash half has no halcompat entry point at all, which is why its
  three gates are restated in halcompat's order.
- **Why not `atLeast()`.** It takes a proxy and would re-read the hash and version from the
  server: a second remote round trip to a server already known to misbehave, at initialization,
  inside the fallback path. That breaks the one-snapshot rule (each value read once, every
  statement a property of that one pair), which stops a predicate result being paired with
  separately retried values and blaming the version rule for a rejection it did not cause. It
  would also change the question, since `atLeast()` gates a named feature release rather than
  this client's own `VERSION`.
- **The decision is unaffected.** The verdict is the public `halcompat::isCompatible<IHdmiCec>()`
  call in `isServiceAvailable()`. The internal entry point is reached only by the post-rejection
  attribution, after the verdict is recorded, and it is `constexpr` over two `int32_t`s with no
  transaction.
- **What would make this public.** Either a public halcompat predicate over an observed version
  pair (the `detail::isCompatible(int32_t, int32_t)` shape at `:108`, promoted), or a public
  accessor returning the hash and version halcompat read while deciding, which would also remove
  the observation's second read. That surface belongs to the halcompat owner in
  `rdk-halif-aidl`, a reference-only consumed header here; this note records the dependency so
  the use is visible to that owner rather than silent.
- See also `describeObservedInterfaceHash()`.

### CCEC::DriverAidlImpl::emitCompatibilityRejectionDiagnostic

- halcompat's public API takes a service proxy, returns a verdict and exposes none of the
  metadata it read; the values it used are locals inside a template in a read-only consumed
  header and cannot be recovered. Reading them again here is the only way the diagnostic can name
  anything about the server, which is why every line is labelled an observation. The full
  enumeration of that surface is under `observedMetadataWouldBeAccepted()`.
- The cause is recorded in `isServiceAvailable()` before this function is entered.
- **One snapshot.** Each value is read exactly once; combining a predicate result with
  separately retried values would let the not-compatible arm hold compatible metadata and blame
  the version rule for a rejection it did not cause.
- **Why both reads are timed.** They are remote round trips to the server just rejected, and a
  server that answers wrongly is the kind that answers slowly or not at all, so a stall here
  happens at initialization, in the fallback path, on a platform already misbehaving. Untimed,
  it would be the one unattributed stall in the file: the last log line would be the rejection,
  with nothing saying the process is blocked in a diagnostic about it.
- **Why several short lines.** `CCEC_LOG` formats through a 500-byte stack buffer (`MAX_LOG_BUFF`
  in `ccec/src/Util.cpp`, not this migration's to change) and `vsnprintf` truncates silently at
  499. One message of about 900 characters would fail in the worst direction: the sentence saying
  no cause is claimed survives at the front while every observed value (the hash, its
  classification, both versions and the recovery note) is cut, leaving a prefix that reads
  complete. Each line is bounded independently with its longest possible substitution, and the
  recovery note is its own line because as a suffix it could push its line over the limit and
  take the values with it. A contract-suite case captures this output, which makes the bound
  observable; any addition must be re-checked against 499.
- A failed observation costs only a less specific message: the caller recorded the cause
  first, and the function is static, so there is no path by which it can relabel an established
  compatibility rejection.

### CCEC::DriverAidlImpl::isServiceAvailable

- Every stage is written not to raise; the catch-all handles the case where one does anyway.
- The custody holder is declared outside the `try`, so the descriptor is released after the
  handler has run rather than during the unwind, keeping the release out of the exceptional
  path's way while still unconditional.
- **Stage 1** passes the two custody out-parameters, which ties the lookup to the node the
  preflight validated as closely as libbinder allows; libbinder still resolves the pathname on
  its own (see `DriverAidlImpl::BinderNodeIdentity`).
- Only the two context-manager pings are bounded. The lookup and the metadata transactions after
  them are synchronous and only timed, which is the residual acquisition window recorded on
  `CCEC::DriverAidlImpl::isServiceAvailable()`.
- **Stage 2.** The preflight's verdict is only worth acting on if it still describes the name
  libbinder is about to open, so the check sits as close to the use as possible, with nothing
  between it and `getService()` but the service name. A failure is reported as an unavailable
  transport because the driver node or its context manager failed, not anything about
  `"HdmiCec"`; reporting a service problem would send an integrator to the wrong place.
- **The not-compatible arm.** `halcompat::isCompatible()` is the single implementation of the
  compatibility rule; nothing re-implements or re-decides it, and recording the cause first fixes
  what explains the fallback by the decision itself.
- **Observation rules.** halcompat decided using its own metadata transactions, whose values
  cannot be recovered. Anything read afterwards is a fresh transaction, and the generated proxy
  does not make the two equivalent: it caches a value read successfully but stores a failed read
  as -1 and retries it, so a transaction that failed for the decision can succeed afterwards, as
  can an in-process fake whose answers change between calls. Three rules follow:
  - one snapshot: the hash and the version are read once each and never combined with a
    re-asked predicate, which could land a still-failing retry alongside compatible values;
  - no causal claim: which of halcompat's three rules rejected the server is not reported,
    because nothing here can know it; all three are named as possibilities, and an observed hash
    that looks frozen does not implicate the era-and-major version rule;
  - no reimplementation of the version rule: it is evaluated by
    `halcompat::detail::isCompatible()`, `constexpr` over two ints, which performs no transaction.
- **Why a separate static function.** On a host with no binder driver the preflight declines
  before this stage, so inline wording could not be reached by a test. Being static is
  load-bearing: with no `this` it cannot reach `availabilityReason`, so the recorded cause
  survives the diagnostic by construction; and because it swallows its own failure, the
  function-level catch, which would record `REASON_QUERY_FAILED` and relabel an established
  rejection, is unreachable from it.

## Logical-address allocation and registration (AIDL back-end)

On the AIDL back-end (`ccec/src/DriverAidlImpl.{hpp,cpp}`), the middleware owns logical-address
allocation. The AIDL HAL leaves it to the client: `IHdmiCec.open()` says every address must be
added by the client, and `hdmi_cec.md` HAL.CEC.9 requires the controller client to allocate as
HDMI 1.4b §10.2 defines and to call `addLogicalAddresses()` only after a successful poll-based
allocation. The legacy back-end is unchanged. Its HAL allocates source addresses inside
`HdmiCecOpen()`.

### `DriverAidlImpl::LOCAL_DEVICE_TYPE`

- A private `static constexpr int` holding `DeviceType::PLAYBACK_DEVICE`. To change the device
  role on the AIDL back-end for a product, change this constant and nothing else.
- The middleware has no device-type configuration, and the Driver interface passes none to
  `open()`. PLAYBACK_DEVICE is the HDMI-source role, and it is the role that the AIDL HAL leaves
  for the client to allocate.
- `getLogicalAddress(int devType)` does not use `devType` to choose an address. It only writes
  the value to the log. The Source plugin calls `LibCCEC::getLogicalAddress(DEV_TYPE_TUNER)`
  with the value 1, which `CCEC::DeviceType` reads as RECORDING_DEVICE. Choosing an address
  from that argument would therefore pick the wrong role.

### `DriverAidlImpl::logicalAddressCandidates(int deviceType)`

A protected static helper. It returns the candidate addresses for a device type in priority
order. The table is the inverse of `LogicalAddress::getType()`
(`ccec/include/ccec/Operands.hpp`):

| `DeviceType`                                           | Candidates      |
|--------------------------------------------------------|-----------------|
| TV                                                     | 0               |
| RECORDING_DEVICE                                       | 1, 2, 9         |
| TUNER                                                  | 3, 6, 7, 10     |
| PLAYBACK_DEVICE                                        | 4, 8, 11        |
| AUDIO_SYSTEM                                           | 5               |
| RESERVED, PURE_CEC_SWITCH, VIDEO_PROCESSOR, any other  | none            |

It is protected rather than private so that a test subclass can exercise it directly. It is
not part of the installed API, because `DriverAidlImpl.hpp` is not installed.

### `DriverAidlImpl::registerDeviceLogicalAddress()`

`open()` calls this helper after the state becomes OPENED, while it still holds the recursive
instance lock. A call to `open()` while the driver is already OPENED returns silently before
reaching this step, so `Bus::start()`'s second `open()` does not allocate again.

The helper first clears the local list, which the new registration replaces. It then settles any
address `unconfirmedReleaseAddress` records. After a successful `close()` there is none, because
`IHdmiCec.close()` removes every added address and `close()` clears the record, so the conforming
path makes no extra HAL call. A record survives only a failed `close()`, which writes the held
address to it, or an add or removal whose outcome the HAL never confirmed. A HAL that accepts the
next `IHdmiCec.open()` may still hold that address, and allocating without releasing it would
leave two registered (the self-poll of a held address goes unanswered, so it reads as free, the
add of it is declined as already added, and the next candidate is added beside it).

| Release step outcome                                             | Action                                                    |
|------------------------------------------------------------------|-----------------------------------------------------------|
| no record                                                        | allocate, with no release and no read-back                |
| `removeLogicalAddresses({held})` ok status, `true`               | record cleared, `LOG_INFO`, allocate                      |
| removal `false` or non-ok, no service proxy held                 | `LOG_EXP`, record kept, nothing registered                |
| removal `false` or non-ok, `IHdmiCec::getLogicalAddresses()` non-ok | `LOG_EXP`, record kept, nothing registered             |
| removal `false` or non-ok, read-back does not list `held`        | treated as released: record cleared, `LOG_INFO`, allocate |
| removal `false` or non-ok, read-back lists `held`                | adopted: local list becomes `{held}`, record cleared, `LOG_EXP`, no poll and no add |
| the removal or the read-back raises                              | the outer handler's `LOG_EXP`, record kept, nothing registered |

Adoption keeps the one-address invariant when the HAL will not let the address go: the HAL holds
exactly that one, `getLogicalAddress()` reads it back, and `isValidLogicalAddress()` agrees. It is
reached only by a HAL that breaks the `IHdmiCec.close()` contract and then refuses the release, so
the adopted address can be one this device's DeviceType would not have chosen; the next
`addLogicalAddress()` replaces it like any registered address. A kept record is settled by the
next `addLogicalAddress()` under the confirmed-release rule, before it adds anything. The removal
and the read-back are timed with the same slow-call diagnostic as every other synchronous AIDL
call.

Allocation then takes each candidate `c` of `LOCAL_DEVICE_TYPE` in order and polls it with this
back-end's own `poll(c, c)`. That call sends a one-byte frame whose initiator equals its
destination, which is the HDMI 1.4b §10.2.1 allocation poll.

| Poll or add outcome                                              | Meaning   | Action                                                    |
|------------------------------------------------------------------|-----------|-----------------------------------------------------------|
| `poll` returns normally (directed `ACK_STATE_0`, or an undocumented send status) | taken | `LOG_DEBUG`, next candidate                 |
| `poll` raises `CECNoAckException` (directed `ACK_STATE_1`)       | free      | `addLogicalAddresses({c})`                                |
| `poll` raises `IOException` (`BUSY`, non-ok status), another `Exception`, a non-CEC `std::exception` or a non-standard exception | not free | `LOG_EXP`, next candidate |
| `addLogicalAddresses` ok status, `true`                          | registered| local list becomes `{c}`, record cleared, `LOG_INFO` address and type, stop |
| `addLogicalAddresses` ok status, `false`                         | declined  | record cleared, `LOG_EXP`, next candidate                 |
| `addLogicalAddresses` non-ok binder status                       | unknown   | `LOG_EXP`, stop with the local list empty; `c` stays in `unconfirmedReleaseAddress`, since the HAL may have applied the add |
| `addLogicalAddresses` raises                                     | unknown   | best-effort `removeLogicalAddresses({c})`, one `LOG_EXP` naming its outcome, stop with the local list empty; `c` stays in `unconfirmedReleaseAddress` unless the removal confirmed it |
| no controller held, or no candidate left                         | none      | `LOG_EXP`, nothing registered                             |
| any other exception (the candidate list, the list node or the request vector failing to allocate) | none | `LOG_EXP`, nothing registered |

**Containment.** Only thread cancellation leaves this step as an exception, so allocation never
makes `open()` raise. Every call that can raise sits inside a handler: the poll's own, the add's
own, and an outer `std::exception`/catch-all pair around the release step and the candidate loop. Each handler's
diagnostic is a constant-format `LOG_EXP` line that allocates nothing, and each catch-all is
preceded by an `abi::__forced_unwind` rethrow so a cancelled thread still unwinds.

**Bookkeeping before the HAL changes.** The one-node `std::list<LogicalAddress>` and the
one-element request vector are built before `addLogicalAddresses()` is called, and `c` is written
to `unconfirmedReleaseAddress` immediately before the call. A successful add is recorded with
`splice()`, which neither allocates nor throws, and the record is cleared, so nothing that can
raise sits between a committed registration and its local record. A declined add added nothing,
so its record is cleared. A non-ok status stops allocation with the record kept, because the HAL
may have applied the add before the transaction failed. An add that raises may also have taken
effect, so it is followed by one timed, compensating `removeLogicalAddresses({c})` inside its own
catch-all, and allocation stops with the local list empty; the record is cleared only when that
removal returns an ok status and `true`, and the `LOG_EXP` line names which removal outcome
occurred. Whenever the record is kept, the next `addLogicalAddress()` releases `c`, or confirms it
absent, before adding, under the confirmed-release rule, so a HAL that kept `c` never ends up
holding a second address.

The pre-OPENED failure arms of `open()` are unchanged: no proxy, a failed `IHdmiCec::open()`, and
a null controller. When allocation leaves the HAL holding no address, `getLogicalAddress()`
reports 0, and `LibCCEC::getLogicalAddress()` then raises its existing `InvalidStateException`.
After a failed add the HAL nonetheless applied, the HAL-backed query reports that address until
the next add or the next registration releases it, or `close()` removes it.

The add call and the compensating removal are timed with the same slow-call diagnostic as every
other synchronous AIDL call.
The pinned libbinder offers no client-side deadline.

### `DriverAidlImpl::open()`

The steps up to OPENED keep the legacy order under one lock: state test, proxy test, threadpool
start, listener construction, `IHdmiCec::open()`, the status and controller tests, then the state
change. The `#if 0` throw is kept verbatim. Address registration runs last. Detail moved out of
the condensed comment:

- **Null controller.** `IHdmiCec.open()` is declared `@nullable` and returns null on error. An ok
  status with no controller is therefore an IOException, not a success.
- **Per-session listener.** A fresh listener is built for each session. Every path that ends a
  session detaches and releases it: both arms of `close()`, both failure arms of `open()`, and
  the destructor. The `eventListener == 0` guard relies on this. A listener reused across
  sessions could still be referenced by the previous session's HAL.
- **Failed-open detach.** A failed open has already handed the listener to the HAL, and nothing
  obliges the HAL to drop it. The listener is detached before the exception leaves.
- **Threadpool.** `IHdmiCecEventListener` is `oneway`, so its callbacks need a binder thread in
  this process. `startThreadPool()` is idempotent. The maximum thread count is left alone, because
  lowering a maximum that is already established can abort the process. `joinThreadPool()` is
  never called, because it would not return.
- **Single client.** `IHdmiCec.open()` fails with `EX_ILLEGAL_STATE` while a session is open. That
  is why the CLOSED/CLOSING/OPENED state machine is kept.

### `DriverAidlImpl::addLogicalAddress()` — exactly one address

The Polaris (AIDL) calls take `int[]`, but the back-end never has more than one address
registered at the HAL. Each array is a one-element temporary.

- **Local bookkeeping and HAL registration are different things.** The local list
  (`logicalAddresses`) is what `isValidLogicalAddress()` answers from; the HAL's registrations are
  what `getLogicalAddress()` reads. A one-entry local list proves nothing about the HAL, so a
  replacement must not add until the old address is known to be gone at the HAL.
- **Exactly-one rule.** Every add or release whose outcome the HAL does not confirm (a `false`
  release, a non-ok status, or an exception from the call) leaves its address in
  `unconfirmedReleaseAddress`, written before the call. That address is settled (released, or
  confirmed absent by a read-back) before any other address is added, so the HAL never holds two.
- The state guard and the controller check are unchanged.
- An address outside `0x0..0xE` (the contract range; 0xF is broadcast) raises
  `AddressNotAvailableException` before any HAL call, so a request the HAL must refuse never costs
  the held address. `LogicalAddress::toInt()` reads one unsigned byte, so only the upper bound can
  be crossed.
- The same address as the local entry returns `true` with no HAL call. A retry of an address that
  is only pending in `unconfirmedReleaseAddress` is released, or confirmed absent, and added again.
- **Confirmed-release rule.** A different address first releases the held one: the local entry,
  or else the address an unconfirmed removal or add left in `unconfirmedReleaseAddress`. It is
  released with `removeLogicalAddresses({old})` while the local record is still in place. An ok
  `true` result confirms the release. A `false` result or a non-ok status is settled by one timed
  `IHdmiCec::getLogicalAddresses()` read. The release counts as confirmed only when that read
  succeeds and does not list the old address. A membership test is the only scan of the result. A
  `false` result alone decides nothing, because the HAL also reports `false` for an address it no
  longer holds.
- **Unconfirmed release.** The old address stays recorded (local entry or record), a `LOG_EXP`
  line names the outcome, nothing is added, and an existing exception is raised:
  - `IOException` when the removal or the read-back failed in transport, or when no service proxy
    is held to read back with;
  - `AddressNotAvailableException` when the HAL answered and still lists the old address.
- **Confirmed release.** The local list is cleared and `source` is written to
  `unconfirmedReleaseAddress`, then `addLogicalAddresses({source})` is called. Success leaves the
  local list exactly `{source}` and clears the record. The node is allocated before any HAL call
  and moved in with `splice()`, so nothing that can fail follows a committed add. A `false` result
  means the HAL added nothing, so the record is cleared and `AddressNotAvailableException` is
  raised. A non-ok status raises `IOException` and an exception from the call propagates; the HAL
  may have applied either, so the record keeps `source`. No failed add is recorded locally, and a
  kept record is settled before the next add, as above.
- **Coarser failure category.** The legacy HAL status separates "address unavailable",
  "general error" and success. `addLogicalAddresses()` returns a single boolean, which is false
  both when the address is out of range and when it is already added. `false` therefore maps to
  the nearer legacy category, `AddressNotAvailableException`.
- `removeLogicalAddress()` keeps its legacy shape and only adds the record described under
  `DriverAidlImpl::unconfirmedReleaseAddress`. `close()` does not clear the local list, and the
  next `open()` registration replaces it.

The Sink plugin allocates its own address and calls `LibCCEC::addLogicalAddress()`
(`HdmiCecSinkImplementation.cpp` :2767 inside a try, :3065 outside any try). The
replace-on-add rule means that this replaces the enable-time address rather than adding a second
one. If the HAL does not confirm the enable-time address released, the Sink's call raises, and
the enable-time address stays the one registered. That is `IOException` on the try's
`IOException` arm, and `AddressNotAvailableException` on its generic arm or uncaught on the
enable-time path.

### `DriverAidlImpl::getLogicalAddress()` — read through the HAL

- Every call goes to `IHdmiCec::getLogicalAddresses()`. The back-end never answers from the local
  list. It returns entry 0, and a result with more than one entry is logged at `LOG_INFO`.
- **Zero is the only "no address" value.** Five cases return 0, and each writes its own log
  line: no proxy, a non-ok status, an empty result, an entry outside 0x0..0xE, and a genuine
  address 0. The legacy back-end also returns 0 when its HAL writes nothing, and
  `LibCCEC::getLogicalAddress()` turns 0 into `InvalidStateException`. Any other sentinel would
  suppress that signal.
- **Raw-value check.** The range check runs on the raw `int32_t` before any conversion. Passing
  the value to `LogicalAddress`'s narrowing constructor would turn 256 into 0x0 and 271 into 0xF,
  which are plausible but wrong addresses. Only a HAL that breaks its contract can reach this
  arm. The legacy back-end has no counterpart: it would hand such a value through unchanged.
- **Logging.** The rejected value is controlled by the HAL. It is logged only through `%d`, and
  never as text or as a format string.

### `DriverAidlImpl::logicalAddresses`

The member is a `std::list`, exactly as in `DriverImpl`, so the two back-ends read alike. On the
AIDL back-end it never holds more than one entry. `isValidLogicalAddress()` reads it, and
`Connection::matchSource()` reads it through that method.

It is local bookkeeping, not the HAL's registration. A replacement is recorded in it only after
the HAL has confirmed the old address released. A removal empties it before the HAL call, as on
legacy, whatever the HAL then answers.

### `DriverAidlImpl::unconfirmedReleaseAddress`

- A private `int`, initialised in class to `LogicalAddress::UNREGISTERED`, meaning "none". It
  holds a single address, never a list, so no multi-address state is introduced.
- It is written immediately before each HAL call that could leave an address registered without a
  local entry, and cleared only when the HAL confirms the outcome:
  - `removeLogicalAddress()` of the held address writes it before the local removal; an ok `true`
    release clears it.
  - Every add writes its address before the call: `addLogicalAddress()` once nothing is held, and
    each enable-time candidate `c` in `registerDeviceLogicalAddress()`. Success or a `false`
    result clears it; a non-ok status or an exception keeps it. An enable-time add that raises
    keeps it unless the compensating `removeLogicalAddresses({c})` returns ok and `true`.
- At most one address is ever pending. An add writes the record only once nothing is held (the
  previous address released or confirmed absent), and a removal writes it only for the address
  already held, so an uncertain address is never overwritten by another.
- `addLogicalAddress()` treats it as the held address when the local list is empty, and releases
  or confirms it under the confirmed-release rule before adding anything.
- A failed `close()` writes the local entry, when there is one, before it raises: the HAL removed
  nothing, so it may still hold that address. With the list empty it leaves the record as it is.
- Besides those confirmed outcomes, it is cleared by a successful `close()`, because
  `IHdmiCec.close()` removes every added address. `registerDeviceLogicalAddress()` settles it
  before allocating, by the same release and read-back as `addLogicalAddress()`, and adopts the
  address instead when the HAL declines the release and still lists it.
- A stale record is self-healing. Releasing an address the HAL no longer holds returns `false`, and
  the read-back then confirms the address absent.

### Test double (`mocks/hdmicec/fake_hdmi_cec_aidl_service.{h,cpp}`)

- `FakeHdmiCecController::sendMessage()` recognises an allocation poll: a one-byte frame whose
  initiator nibble equals its destination nibble.
  - The poll is answered `ACK_STATE_1` (free) unless `setLogicalAddressOccupied(address, true)`
    marks the address taken (`ACK_STATE_0`). `setAllocationPollResult()` can install any other
    status.
  - Polls are recorded in `getAllocationPolls()` and counted by `getTotalSendMessageCallCount()`;
    `getSendMessageCallCount()` and the last-sent capture still describe application frames only.
  - A non-ok status installed with `setSendMessageBinderStatus()` fails polls too, with the
    out-parameter unwritten.
- A successful `addLogicalAddresses` / `removeLogicalAddresses` (ok status, `true`) updates
  `getRegisteredLogicalAddresses()`. A successful `IHdmiCec::close()` and
  `FakeHdmiCecController::reset()` both clear it.
  - By default both calls validate the whole request as `IHdmiCecController.aidl` requires before
    changing anything: an add reports `false` when any address is outside 0..14 or already
    registered, and a removal when any address is outside 0..14 or not registered.
  - `setAddLogicalAddressesResult()` / `setRemoveLogicalAddressesResult()` force a result instead
    until `reset()`.
- `FakeHdmiCecService::getLogicalAddresses()` by default reports the controller's registered
  addresses. A vector installed with `setLogicalAddressesResult()` still overrides that default.
- The out-of-process host (`fake_hdmi_cec_aidl_service_host.cpp`) uses the same fake. Its control
  verb `registered` replies `OK registered <decimal,...>`; the field is empty when nothing is
  registered. Its verb `calls` replies with the binder transactions the fake service and its
  controller have received, per AIDL method, counted in the fake's `onTransact()`; allocation
  polls count under `IHdmiCecController.sendMessage`.

### Tests

- **`DriverAidlLocalInstanceTest`** (any invocation; uses locally injected fakes). Covers:
  - the candidate table;
  - enable registering `{4}` and reading it back through the HAL;
  - occupied candidates (4 taken gives 8; 4 and 8 taken gives 11; all taken gives none);
  - a busy poll, a declined add, a transport failure on add, and a null controller;
  - containment, through test-local controller doubles: a poll raising a standard or a
    non-standard exception (candidate treated as taken), an add raising `std::bad_alloc` before
    registering, an add raising after registering (withdrawn by the compensating removal), and a
    compensating removal that raises as well;
  - a compensating removal that raises, reports false or fails with DEAD_OBJECT, whether the HAL
    kept the candidate or never registered it. In each case the next add releases the candidate,
    or confirms it absent through the read-back, before adding, and a successful close drops the
    record (`AnUnconfirmedCompensatingRemovalIsSettledBeforeTheNextAddressIsAdded`);
  - replace-on-add and the same-address no-op. These run through `JournalingControllerDouble`, a
    test-local controller that journals each add and remove in order and forwards it to the
    service's own fake. The journal yields the exact remove-then-add sequence, and the most
    addresses the HAL held after any call;
  - every unconfirmed-release arm, each checked against the HAL's registrations, the HAL-backed
    `getLogicalAddress()` and `isValidLogicalAddress()`. The arms are a DEAD_OBJECT release, a
    declined release the HAL still lists, a declined release it no longer lists, a failed
    read-back, and no service proxy (`AddingADifferentAddressReplacesTheRegisteredOne`,
    `AReleaseThatCannotBeReadBackRaisesIoExceptionAndAddsNothing`);
  - an address outside 0x0..0xE refused with no HAL call and the held address kept
    (`AnOutOfRangeAddressIsRefusedBeforeTheHeldOneIsReleased`);
  - a failed or declined standalone removal settled by the next add, so at most one address is
    ever registered (`AnUnconfirmedRemovalIsSettledBeforeTheNextAddressIsAdded`);
  - every add or removal whose outcome is not confirmed, through `UnconfirmedOutcomeControllerDouble`,
    a `JournalingControllerDouble` whose next add or removal raises `std::bad_alloc` or returns
    `FAILED_TRANSACTION`, before or after the fake applies it, and is journalled as `op!{a}`. A
    standalone removal, an enable-time add and an explicit add each leave their address for the
    next add to release first (in between, a confirmed removal of `TUNER_1`, which the test
    registers on the fake directly as another client would and this back-end does not hold,
    leaves it pending), and a declined add, explicit or at enable, leaves nothing to release
    (`ARemovalThatRaisesOrFailsIsReleasedAgainBeforeTheNextAdd`,
    `AnEnableTimeAddThatFailsInTransportIsReleasedBeforeTheNextAdd`,
    `AnExplicitAddThatRaisesOrFailsIsReleasedBeforeTheNextAdd`,
    `ADeclinedAddLeavesNothingForTheNextAddToRelease`). After every back-end call the fake holds at
    most one address, and the HAL-backed `getLogicalAddress()`, the local list and
    `isValidLogicalAddress()` are each checked;
  - an allocation failure at each global `operator new` call of an unfailed enable-time allocation
    in turn, injected by a replacement `operator new` in the test file that a thread-local
    countdown arms (`ScopedAllocationFailure`) and that behaves as the default otherwise. For every
    call nothing escapes, the driver stays OPENED, the fake holds at most one address, the local
    list is empty or equal to it, and an address the HAL kept without a local entry is released by
    the next add before it adds; the failures before any add are the ones the outer
    `std::exception` handler contains (`AnAllocationFailureAnywhereInEnableTimeAllocationIsContained`);
  - close followed by re-registration;
  - a re-open after a failed close, whose fake keeps its registrations. The kept address is
    released before allocating, whether 4 is then free or occupied and whether it was registered
    or left by an unconfirmed enable-time add, so the journal reads `remove{4} add{c}` and the HAL
    holds only `{c}` (`AReopenAfterAFailedCloseReleasesTheKeptAddressBeforeAllocating`). A
    declined release is adopted when the HAL still lists it and released when it does not
    (`AReopenWhoseReleaseIsDeclinedFollowsTheHalsOwnAddressList`). A release that fails with a
    failed read-back, lacks a service proxy, or raises registers nothing and keeps the record for
    the next add (`AReopenWhoseReleaseCannotBeConfirmedRegistersNothingUntilTheNextAdd`). With
    nothing recorded, after a successful close or a failed one holding no address, the re-open
    makes no removal and no read-back
    (`AReopenWithNothingRecordedReleasesNothingBeforeAllocating`).
- **`DriverAidlSessionTest`** (invocation B). Covers:
  - enable registering exactly `{4}`;
  - `LibCCEC::getLogicalAddress(1)` returning 4 through the HAL;
  - re-open with 4 occupied registering `{8}`;
  - no free candidate leading to `InvalidStateException`;
  - a replacement releasing `{4}` before adding `{8}`. The order is read from the fake's own
    per-call log lines (`AddLogicalAddressMarshalsExactlyOneElement`).

  The fixture's TearDown runs close and then open after resetting the fake. This re-registers
  the enable-time address, so the driver and the fake agree before the next case.
- **`DualPathAidlFlowTest.EnablingTheDriverRegistersOneAddressThatLibCcecReadsBackThroughTheHal`**
  (invocation E, real binder IPC). The host reports `registered` = `4`, and
  `LibCCEC::getLogicalAddress(1)` returns 4. The host's `calls` counts, read before and after that
  call, show exactly one more `IHdmiCec.getLogicalAddresses` transaction and every other count
  unchanged, so the address came through the HAL and not from a cache.

### Superseded pre-refine design

Before this change, `open()` registered no address, `getLogicalAddress()` ignored `devType` and
returned whatever the HAL held unprompted, and the fake answered a canned single entry 4.

## Physical address (AIDL back-end)

### Value and encoding

`DriverAidlImpl::getPhysicalAddress()` writes `DriverAidlImpl::FIXED_PHYSICAL_ADDRESS`, which is
`0x01000000`: the physical address 1.0.0.0, one nibble per byte, most significant first. The value
is fixed because the `com.rdk.hal.hdmicec` AIDL HAL exposes no physical-address query.

The encoding is the one the production callers of `LibCCEC::getPhysicalAddress()` decode. Both
plugins seed `0x0F0F0F0F` (F.F.F.F), call the method, and split the result into four bytes that
they pass to `PhysicalAddress(b0, b1, b2, b3)`:

| Caller | Lines | Decode |
|---|---|---|
| `entservices-hdmicecsource/plugin/HdmiCecSourceImplementation.cpp` | 1176-1177 | `{(v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF}` |
| `entservices-hdmicecsink/plugin/HdmiCecSinkImplementation.cpp` | 3201-3202 | identical |

`PhysicalAddress::toString()` then renders `0x01000000` as `"1.0.0.0"`. The packed 16-bit form
`0x1000` would decode there as 0.0.0.0, which is why it is not used.

### No HAL call

- The method calls nothing on `IHdmiCec` or `IHdmiCecController` and no legacy `HdmiCec*`
  function, and it reads neither the service proxy nor the controller reference.
- It has no state guard, matching `DriverImpl::getPhysicalAddress()`, so a driver that was never
  opened, an OPENED driver and a CLOSED driver all answer 1.0.0.0.
- It takes no lock. `write()` holds the instance lock across the synchronous
  `IHdmiCecController::sendMessage()`, so a locked query would wait on that AIDL call; this one
  answers while a transmit is stalled in the HAL.
- `LibCCEC::getPhysicalAddress()` is unchanged and still raises `InvalidStateException` before
  the library is initialized; once initialized with the AIDL back-end selected, it returns
  `0x01000000`.
- The legacy back-end is unchanged: `DriverImpl::getPhysicalAddress()` still reads the address
  with `HdmiCecGetPhysicalAddress()`.

### Null out-parameter

A null `physicalAddress` is logged at `LOG_DEBUG` and not written; the method returns without
raising. A non-null one is written and the value is logged at `LOG_DEBUG`.

### Tests

| Case | Invocation | Asserts |
|---|---|---|
| `DriverAidlLocalInstanceTest.GetPhysicalAddressReportsTheFixedAddressOnANeverOpenedDriver` | A, B, C | `0x0F0F0F0F` becomes `0x01000000` and decodes to `"1.0.0.0"` on three calls; no legacy HAL call |
| `DriverAidlLocalInstanceTest.GetPhysicalAddressToleratesANullOutParameter` | A, B, C | a null pointer neither crashes nor raises, and the next query still answers |
| `DriverAidlLocalInstanceTest.GetPhysicalAddressIsFixedInEveryStateAndCallsNoAidlMethod` | A, B, C | same value while OPENED and after `close()`; zero calls on call-counting service and controller doubles |
| `DriverAidlLocalInstanceTest.GetPhysicalAddressAnswersWhileATransmitIsStalledInTheHal` | A, B, C | the query completes while another thread's `sendMessage()` holds the instance lock |
| `DriverAidlSessionTest.LibCCECReportsTheFixedPhysicalAddressWithoutAnyAidlCall` | B | `LibCCEC::getPhysicalAddress()` yields `0x01000000`; every fake service and controller counter unchanged |
| `DualPathAidlFlowTest.LibCCECReportsTheFixedPhysicalAddressWithoutCrossingBinder` | E (skips on D) | same over real binder IPC; every IHdmiCec and IHdmiCecController transaction count the host's `calls` reports unchanged across the query |

`DriverAidlLegacyArmTest.PhysicalAddressIsReadThroughTheLegacyHalApi` still covers the legacy
back-end reading the address through the legacy HAL.

### Limitation

The value is correct only where the device's real position in the HDMI topology is 1.0.0.0, such
as a source connected directly to the first input of the root display. A device behind a switch
or an AV receiver, or on another input, reports 1.0.0.0 anyway, and every CEC message that
carries the physical address (`<Report Physical Address>`, `<Active Source>`) carries that value.
Reporting the real topology position needs a source the AIDL HAL does not provide; the
device-settings EDID read is the candidate.

### Superseded interim design (B1)

Before the address was fixed, the AIDL back-end treated physical-address retrieval as blocked
item B1: the method logged `BLOCKED ITEM B1` at `LOG_EXP` and left the caller's out-parameter
untouched, pending the device-settings HAL header declaring the EDID-byte read, which was to be
supplied as a separate input and was not. That design rejected a lazily opened legacy CEC handle
(a CEC HAL substitution that would also run source logical-address discovery on the wire beside
the active AIDL controller), the AIDL HDMI-output EDID event, and reconstructing the declaration
from test mocks. The fixed 1.0.0.0 replaces it, B1 no longer blocks the AIDL path, and the case
`DriverAidlLocalInstanceTest.GetPhysicalAddressIsBlockedOnB1AndLeavesTheOutParameterUntouched`
was replaced by the cases above.

## mocks/hdmicec/fake_hdmi_cec_aidl_service.h

Detail moved out of the condensed Doxygen in the fake AIDL service header. Each entry names the
symbol whose comment it came from.

### Superseded statements

- The one-logical-address-per-device change (the middleware derives the address from its
  DeviceType, discovers it by polling and registers it with `addLogicalAddresses` when the driver
  is enabled) makes several pre-refine statements about the fake untrue, so they were removed and
  not carried forward: that `getLogicalAddresses()` answers a copy of a canned one-entry
  `{ DEFAULT_LOGICAL_ADDRESS }` vector by default (stated on `DEFAULT_LOGICAL_ADDRESS`, on the
  `getLogicalAddresses()` out-parameter, on the `setLogicalAddressesResult()` post-condition and
  on the `logicalAddressesResult` member); that
  the controller is "intentionally dumb", performs no CEC reasoning and never inspects a frame's
  destination nibble; that `sendMessage()` never examines the message bytes and reports its canned
  `SendMessageStatus` with no opinion of its own; and that `addLogicalAddresses()` never interprets
  the vector it captures. Under that change the fake's `getLogicalAddresses()` by default reflects
  the addresses registered through its controller (canned overrides remain possible), and the
  controller answers a self-addressed one-byte poll as not acknowledged unless a test marks that
  address occupied.

### HDMI_CEC_MOCKS (group)

- The doubles cover two transports: the legacy in-process C ABI, stood in for by the GoogleMock
  double in `hdmi_cec_driver_mock.h`, and the out-of-process `com.rdk.hal.hdmicec` AIDL HAL, stood
  in for by the fake. Because no production source list references the directory, nothing in it
  can reach the shipped middleware library.

### HDMI_CEC_FAKE_AIDL_SERVICE (group)

- The fakes implement the existing interface exactly as the frozen 0.1.0.0 snapshot declares it:
  no `.aidl` is authored, no interface is added and no method is added to an interface. Both
  classes implement the methods the generated bases leave pure virtual — the seven declared by
  `IHdmiCec` and the three declared by `IHdmiCecController`.
- `getInterfaceVersion()` and `getInterfaceHash()` are pure virtual on the two interfaces but
  concrete on `BnHdmiCec` and `BnHdmiCecController`, which answer them from the snapshot's
  compiled-in version and hash, so deriving from a generated base already satisfies them. The fake
  overrides that concrete pair deliberately: overriding it is the only way to make a fake report
  metadata the middleware has to refuse, and so the only way to reach the present-but-incompatible
  arm of the middleware's back-end selection.
- Registration under `IHdmiCec::serviceName()` is what makes the middleware's own service lookup,
  and therefore its runtime back-end selection, reach the fakes.
- In-process dispatch: libbinder resolves a name registered in the calling process to the local
  `BBinder`, so `interface_cast` hands back the fake object itself and every call, including the
  metadata pair, dispatches virtually to it. This is the only mode in which the four settable
  metadata values (each class's hash and version) take effect. Both of the service's values can
  change the compatibility verdict, and so a selection outcome, because the middleware's
  compatibility check reads the service interface's hash and version; the controller's pair never
  takes part. The harness's `incompatible` mode varies only the service's hash (it installs
  `"-1"`), which is that mode's choice rather than a limit of the version control.
- Out-of-process dispatch: the client holds a real proxy, transactions cross the binder driver and
  event callbacks arrive on the client's binder threadpool. The generated `onTransact()` answers
  the metadata transactions from the compiled-in constants, so the metadata overrides are inert.

### File header (`@file`)

- The header also declares the address-free trace label by which every diagnostic of the fake
  identifies an object. Every class is a hand-written fake with plain settable canned responses
  and plain virtual overrides rather than a GoogleMock double.

### fakeHdmiCecTraceLabel()

- It is the single spelling of object identity in every diagnostic the fake and its host binary
  print.
- A raw pointer value answers none of the questions a reader of those lines has — is an object
  held at all, is it the same one as the line above, how many have there been — while changing on
  every run, which turns a diff of two captured logs into noise and discloses the process's address
  layout for no diagnostic gain. A label answers all three and does neither.
- Presence is reported as a word. Identity is reported as an ordinal minted from one process-wide
  sequence on first sight of an object and reported for that object thereafter, so "listener #1"
  and "listener #2" tell a re-registration from a repeat; a number denotes exactly one object for
  the life of the process whatever kind of object it is, and no two objects share one.
- The `object` argument is used as an identity key only; its value is never printed and never
  returned. A later call for the same object returns the same label.
- Ordinals depend on the order in which a run traced its objects, so they are stable within a
  capture and deliberately not across captures.
- Identity is keyed on the address, which is never disclosed, and the registry is never pruned, so
  an object destroyed and another allocated at the same address are reported under one ordinal.
  Carrying the ordinal as a member instead would mean widening the generated server bases, which
  the fake may not do, and the objects traced are a session's own and outlive the lines that name
  them.
- An ordinal belongs to a pointer value, so one object reached through two different base pointers
  of a multiply-inherited type can be minted two of them. Every site in the fake and its host traces
  an object through a single static type — a listener always as `IHdmiCecEventListener`, the
  service always as `FakeHdmiCecService` — which keeps one object under one ordinal, and a new site
  has to do the same.

### FakeHdmiCecController

- It is the controller half of the fake HAL, through which a client adds and removes logical
  addresses and transmits CEC messages.
- It derives from `BnHdmiCecController` and deliberately not from `IHdmiCecControllerDefault`,
  because `IHdmiCecControllerDefault::onAsBinder()` returns nullptr: an object derived from it can
  never be published to the service manager nor reached through a proxy, so a fake built on it
  would silently never be called.
- Every canned response has a deterministic default stated on its setter, so a test that
  configures nothing still observes a fully functional controller. Translation logic (status
  mapping, exception mapping) belongs to the middleware adapter under test; duplicating it in the
  fake would make a test assert the fake's opinion instead of the adapter's behaviour.

### FakeHdmiCecController::addLogicalAddresses()

- The captured vector's width is exactly what the caller marshalled, which is the property the
  single-element-array assertions rely on.
- A non-ok canned status is returned unchanged and before the out-parameter is written; the adapter
  under test must map it to `IOException`.
- `setAddLogicalAddressesResult()` and `setAddLogicalAddressesBinderStatus()` select the outcome.
  Neither call is required: by default the status is ok and the result is the contract's
  validation of the request, `true` only when every address is in 0..14 and none is already
  registered.
- Every call, whatever its outcome, advances `getAddLogicalAddressesCallCount()` and replaces
  `getLastAddedLogicalAddresses()`. Only an ok status with a `true` result registers the
  addresses; a non-ok status leaves both the out-parameter and the registrations untouched.

### FakeHdmiCecController::removeLogicalAddresses()

- A non-ok canned status is returned unchanged and before the out-parameter is written; the adapter
  under test must log it and let nothing escape.

### FakeHdmiCecController::sendMessage()

- Every call advances `getTotalSendMessageCallCount()`. An allocation poll is a one-byte frame
  whose initiator nibble equals its destination nibble: it is recorded in `getAllocationPolls()`
  and answered from `setLogicalAddressOccupied()` / `setAllocationPollResult()`, `ACK_STATE_1`
  (free) by default. A poll never advances `getSendMessageCallCount()`, never replaces
  `getLastSentMessage()` and never reports the `setSendMessageResult()` status.
- Any other frame is an application frame: it advances `getSendMessageCallCount()` and is captured
  whole for `getLastSentMessage()` before the binder status is examined, then answered with the
  `setSendMessageResult()` status, `ACK_STATE_0` by default.
- A non-ok canned status is returned unchanged and before the out-parameter is written; the adapter
  under test must map it to `IOException` whatever send status was also installed. Allocation polls
  get the same status, so an injected transport failure also reaches the allocation.
- Because no length limit is applied, a frame the adapter was required to reject leaves the
  application-frame count and capture unchanged and is thereby distinguishable from one it truncated.

### FakeHdmiCecController::getInterfaceVersion()

- `BnHdmiCecController` already implements the method concretely from the same compiled-in
  `IHdmiCecController::VERSION`, so the override is not required. It is kept so both metadata
  methods on both fake classes answer from a member that `setInterfaceVersion()` installs and
  `reset()` restores, giving the pair one shape and one place to restore.
- What the value can decide is narrower than the service interface's version: the middleware's
  compatibility check reads the service interface's metadata alone.

### FakeHdmiCecController::getInterfaceHash()

- It overrides the concrete implementation `BnHdmiCecController` supplies. It is no more required
  than the version override and is settable on exactly the same terms.

### FakeHdmiCecController::onTransact() and getTransactionCounts()

- The count is taken at the transport because no method-level counter sees every call: a remote
  `getInterfaceVersion()` / `getInterfaceHash()` is answered by the generated
  `BnHdmiCecController::onTransact()` from compiled-in constants without reaching the fake's
  virtual methods, and allocation polls bypass `getSendMessageCallCount()`. Every transaction the
  generated dispatch receives is counted under its code, including codes no AIDL method uses.
- The lock is released before the generated dispatch runs, because the method it dispatches to
  takes the same non-recursive mutex.
- `BBinder::transact()`, which is final, answers `PING_TRANSACTION`, `EXTENSION_TRANSACTION`,
  `DEBUG_PID_TRANSACTION` and `SET_RPC_CLIENT_TRANSACTION` itself before `onTransact()` is
  reached, so those framework transactions, none of them an AIDL method, are never counted. Under
  local (in-process) dispatch the methods are called directly and nothing is counted. `reset()`
  clears the counts.
- The out-of-process host reports the counts through its `calls` command.

### FakeHdmiCecController::setAddLogicalAddressesResult()

- The AIDL HAL collapses the legacy HAL's distinction between "logical address unavailable" and
  "general error" into one boolean; `false` is the value the adapter must translate into
  `AddressNotAvailableException`, the caller-visible difference the migration registers and
  exercises through both real Sink call paths.

### FakeHdmiCecController::setRemoveLogicalAddressesResult()

- The legacy back-end discards the HAL's removal return value and returns silently, so raising on
  a false result would be an unregistered behaviour difference; a false result is how a test
  proves the adapter does not raise.

### FakeHdmiCecController::setAddLogicalAddressesDelayMs()

- The middleware's slow-call diagnostic is a threshold, not a timeout: `DriverAidlImpl` brackets
  every synchronous AIDL call with a monotonic clock read and emits one `LOG_WARN` line when the
  call outlives its threshold, abandoning nothing and raising nothing. No canned result or status
  takes time, so only a genuinely slow call reaches that arm; a test that asserted the warning
  without making a call slow would be asserting nothing.
- `addLogicalAddresses()` carries the delay because it is the shortest complete round trip a test
  can drive through the public `Driver` interface, so the delay is paid once and no session state
  changes around it.
- The delay value is read under the instance mutex and the lock is dropped before sleeping:
  holding it across the sleep would stall a concurrent capture read on a remote fake answering on
  binder threads.
- The sleep is real wall-clock time paid in test duration, so keep it just above the middleware's
  threshold rather than comfortably above it.

### FakeHdmiCecController::setSendMessageResult()

- `ACK_STATE_0` means acknowledged for a directed message but rejected for a broadcast,
  `ACK_STATE_1` is the mirror of that, and `BUSY` means arbitration failed and nothing was sent.
- It applies to application frames only. Allocation polls keep their own default, `ACK_STATE_1`
  (free), which `setAllocationPollResult()` and `setLogicalAddressOccupied()` change per address.

### FakeHdmiCecController::setAddLogicalAddressesBinderStatus()

- Each per-method binder status is independent of those installed for the other methods, so a
  test can fail one call without disturbing the rest.

### FakeHdmiCecController::setRemoveLogicalAddressesBinderStatus()

- Unlike the add case, the adapter must log and swallow a removal transport failure rather than
  raise, mirroring the legacy back-end's silent return.

### FakeHdmiCecController::setSendMessageBinderStatus()

- The adapter must translate a non-ok status into `IOException` regardless of any
  `SendMessageStatus` value installed.
- The status also fails allocation polls, which the middleware's allocation treats as "not free"
  and skips, so enabling with it installed registers no address.

### FakeHdmiCecController::setInterfaceHash()

- Its reach is narrower than the service's: the middleware's compatibility check reads the service
  interface's (`IHdmiCec`) metadata alone, so a hash installed here decides no selection outcome
  and must not be read as a second route to the present-but-incompatible arm. It makes the value
  this class reports observable and changeable, so the divergence trace on `getInterfaceHash()` is
  reached by a test rather than left as an unreachable defensive branch.
- The parameter is an arbitrary string, exactly as on the service's setter, because no value is
  privileged here.
- The default `IHdmiCecController::HASHVALUE` is the real frozen hash compiled into the snapshot,
  so an unconfigured controller reports the compatible value; `reset()` restores it so an override
  cannot leak into the next case.
- A remotely served fake cannot report divergent metadata at all, because the generated
  `onTransact()` answers the metadata transactions from the compiled-in constants, so the setter has
  no effect out of process and must not be judged redundant on the evidence of a remote run
  ignoring it.

### FakeHdmiCecController::setInterfaceVersion()

- It is the version half of the same pair, on the same terms and with the same reach: the
  compatibility check never reads it, and its job is to make the divergence trace on
  `getInterfaceVersion()` reachable and reached.
- No validation is applied — any `int32_t` is installed as given — because the point of the
  control is to report a value the snapshot would not.
- The default `IHdmiCecController::VERSION` is the version compiled into the snapshot, so an
  unconfigured controller reports the compatible value.

### FakeHdmiCecController::getLastAddedLogicalAddresses()

- The middleware's address operations are single-valued, so a correct adapter marshals exactly one
  entry carrying exactly the requested address; without this capture that assertion cannot be
  written.

### FakeHdmiCecController::getLastRemovedLogicalAddresses()

- It asserts the same single-element marshalling contract on the removal side.

### FakeHdmiCecController::getLastSentMessage()

- Truncation would silently put a corrupt CEC frame on the bus, so the length-boundary tests
  distinguish "rejected" from "trimmed" by reading this capture.
- It holds application frames only. Allocation polls are recorded in `getAllocationPolls()` and
  never replace it, so the polls the driver makes when enabled leave it as it was.

### FakeHdmiCecController::getSendMessageCallCount()

- It counts application frames only, not every `sendMessage()` call: an allocation poll is
  recorded in `getAllocationPolls()` instead, and `getTotalSendMessageCallCount()` counts every
  call, polls included.
- It is the assertion target for "the adapter rejected this frame without transmitting": a guard
  that fires before the HAL call leaves the counter unchanged, which tells a rejection apart from a
  failed transmit.

### FakeHdmiCecController::getTotalSendMessageCallCount()

- It counts every `sendMessage()` call, allocation polls included, so a no-call snapshot built on
  it also catches an illicit poll, which `getSendMessageCallCount()` cannot see. It equals
  `getSendMessageCallCount()` plus the size of `getAllocationPolls()`.
- `getSendMessageCallCount()` stays the application-frame count the transmit assertions read, so
  the poll the driver makes when enabled does not disturb them; the host's `sent-count` verb
  reports that application-frame count and no host verb reports this total.

### FakeHdmiCecController::reset()

- The service resolves once per process, so the fake cannot be re-registered per case; `reset()`
  is what keeps configuration and observations from leaking from one case into the next.

### FakeHdmiCecController::mutex

- All critical sections are short and hold no lock across a callback, because a remote fake
  answers on binder threads while a test thread may be reading a capture or installing a canned
  response.

### FakeHdmiCecController::addLogicalAddressesDelayMs

- The default of 0 means no case pays for the delay unless it asks.

### FakeHdmiCecController::interfaceVersionResult

- Written by its initialiser, by `setInterfaceVersion()` and by `reset()`. It is a member rather
  than a literal in the getter so that `reset()` has one place to restore and the getter has one
  value to return.

### FakeHdmiCecController::interfaceHashResult

- Written by its initialiser, by `setInterfaceHash()` and by `reset()`, exactly as the version is.

### FakeHdmiCecService

- It is the service half of the fake HAL: the object found by the middleware's own service lookup.
- It derives from `BnHdmiCec` and deliberately not from `IHdmiCecDefault`, because
  `IHdmiCecDefault::onAsBinder()` returns nullptr and an object derived from it cannot be
  published to the service manager at all.
- It records what it was given, counts its calls and answers with canned responses, and runs no
  state machine of its own: the middleware tracks its own open and closed states, and a second
  source of truth in the fake would let a test pass against the fake's opinion rather than the
  adapter's behaviour.
- It hands out its controller from `open()` and never replaces it, so a test may configure the
  controller before or after the middleware opens the session. Nothing fires spontaneously.
- All seven interface methods are declared because the generated interface declares them pure
  virtual. Four of them — `getState()`, `getProperty()`, `registerEventListener()` and
  `unregisterEventListener()` — are deliberately not consumed by the middleware, and their call
  counters exist so a test can assert exactly that.

### FakeHdmiCecService::DEFAULT_STATE

- A started service is the only state consistent with a fake that is published and answering. It
  is a constant and not a canned response because the middleware never calls `getState()`, so a
  setter for it could not change any behaviour under test.
- The two-valued AIDL `State` enum it is drawn from is unrelated to the middleware's own closed,
  closing and opened states. `getState()` is the one method that reports it.

### FakeHdmiCecService::~FakeHdmiCecService()

- Clearing the instance pointer ensures a destroyed fake can never be handed out by
  `getInstance()`.

### FakeHdmiCecService::getState()

- The method has no failure arm to configure, because nothing under test calls it; it answers a
  fixed state with a fixed status and advances its call counter.
- The middleware tracks its own closed, closing and opened states, and consulting the HAL's would
  create a second source of truth.

### FakeHdmiCecService::getProperty()

- An empty optional is a valid "property not available" answer. No property the interface
  publishes has a legacy counterpart, so none is consumed: reading the HAL's CEC version or its
  transmit metrics would be new behaviour with no legacy counterpart.
- The method has no failure arm to configure, because nothing under test calls it.

### FakeHdmiCecService::getLogicalAddresses()

- A non-ok canned status is returned unchanged and before the out-parameter is written; the adapter
  under test must report it as no address available.

### FakeHdmiCecService::open()

- Capturing the listener is what makes the three triggers deliver; from that point on they stop
  being no-ops.
- The parameter name `cecControllerListener` is the generated one; its type is
  `IHdmiCecEventListener`.
- A non-ok canned status is also how the `EX_ILLEGAL_STATE` the interface documents for an
  already-open service is expressed, since the fake runs no state machine that could raise it.
- The listener is captured even when a null controller or a non-ok status is reported, so the
  receive path can be exercised against a session the adapter rejected.

### FakeHdmiCecService::close()

- Leaving the captured listener in place is exactly what the "callback arriving during or after a
  close is rejected by the state guard" case needs.
- On a non-ok status the adapter under test must still reach its own closed state before raising.

### FakeHdmiCecService::registerEventListener()

- The listener is not retained, because the fake delivers events only to the listener captured by
  `open()`. The middleware is the controlling client and receives events through the listener it
  passes to `open()`; this method exists for non-controlling clients and to satisfy the
  pure-virtual interface. It has no failure arm to configure, because nothing under test calls it.

### FakeHdmiCecService::unregisterEventListener()

- It has no failure arm to configure, because nothing under test calls it, and is not consumed for
  the same reason as `registerEventListener()`.

### FakeHdmiCecService::getInterfaceVersion()

- `BnHdmiCec` already implements the method concretely from the same compiled-in
  `IHdmiCec::VERSION`, so the override is not what satisfies the pure-virtual declaration; it exists
  so that the version is answered from a member alongside the hash, both installable and both
  restored together.
- The harness's incompatible mode drives the hash; the version arms of the compatibility check are
  covered at unit level by locally constructed doubles.

### FakeHdmiCecService::getInterfaceHash()

- This is the metadata answer the suite has the strongest reason to divert — it is what the
  harness's incompatible mode installs — and overriding the concrete `BnHdmiCec` implementation is
  the only way to divert it.

### FakeHdmiCecService::onTransact() and getTransactionCounts()

- The same transport-level count as `FakeHdmiCecController::onTransact()`, dispatching through
  `BnHdmiCec::onTransact()`: every incoming transaction is counted under its code, remote metadata
  calls included, with the lock released before dispatch; the framework transactions
  `BBinder::transact()` answers itself (`PING_TRANSACTION` among them) and local dispatch are not
  counted, and `reset()` clears the counts without touching the controller's.
- It is the evidence behind the L2 D2 and D3 cases: exactly one `IHdmiCec.getLogicalAddresses`
  across a logical-address read, and no transaction at all across a physical-address query.

### FakeHdmiCecService::setLogicalAddressesResult()

- For an empty vector the adapter must report no address, matching the legacy back-end, whose
  query leaves its zero-initialised local untouched.
- For more than one entry the adapter must log the count and operate on the first entry only,
  adding no multi-address state, no iteration and no dispatch fan-out.

### FakeHdmiCecService::setCloseResult()

- Completing the adapter's own transition to closed before raising ensures a subsequent open is
  not blocked by a half-closed session.

### FakeHdmiCecService::setOpenReturnsNullController()

- An ok status with no controller is a HAL that answered successfully and returned nothing usable;
  the adapter must raise `IOException` rather than store a null session and fail later on first
  use. The combination is only producible by asking for it here.

### FakeHdmiCecService::setOpenBinderStatus()

- The adapter must translate a non-ok open status into `IOException`. `EX_ILLEGAL_STATE` is
  expressed here because the fake runs no state machine that could raise it by itself.

### FakeHdmiCecService::setGetLogicalAddressesBinderStatus()

- A failed query has the same observable outcome as a successful but empty one; the two are
  distinguished in the adapter's log rather than in its return value.

### FakeHdmiCecService (no setters for the unconsumed methods)

- There is deliberately no response or status setter for `getState()`, `getProperty()`,
  `registerEventListener()` or `unregisterEventListener()`: a control that varied what they answer
  could not change any behaviour under test, and its only effect would be to suggest coverage that
  does not exist. "The adapter never called this" is a real assertion, and a counter that must stay
  zero is how it is written.

### FakeHdmiCecService::setInterfaceHash()

- It is the metadata control with a production-path consumer: it publishes a service the middleware
  must find and then refuse, so the factory-level fallback from a present but incompatible service
  can be observed. The parameter is an arbitrary string because the value that produces that
  outcome belongs to the harness rather than to the fake.
- The default `IHdmiCec::HASHVALUE` is the real frozen hash compiled into the snapshot, so an
  unconfigured fake is the compatible case; `reset()` restores it so an override cannot leak into
  the next case.
- A remotely served fake cannot report divergent metadata, because the generated `onTransact()`
  answers the metadata transactions from the compiled-in constants, so the setter has no effect on
  the out-of-process invocation and must not be judged redundant on the evidence of a remote run
  ignoring it.
- The one consumer that changes a selection outcome is the L1 harness in
  `tests/L1Tests/test_main.cpp`, whose `incompatible` mode installs the broken hash `"-1"` before
  the middleware's selection resolves. The other rejection arms — the empty hash, the `"notfrozen"`
  development hash and every version arm — are covered at unit level by locally constructed doubles
  in `tests/L1Tests/ccec/test_DriverAidl.cpp`; no harness mode installs those values, so nothing
  reaches those arms through this setter. The same file also drives this setter
  and the other three metadata setters directly on locally constructed, never-registered fakes,
  which makes each getter's divergence trace a reached branch rather than a reachable one.

### FakeHdmiCecService::setInterfaceVersion()

- Its reach is deliberately smaller than the hash's: it decides no selection outcome under test.
  The compatibility check does read this interface's version, but every version arm is exercised at
  unit level by locally constructed doubles rather than through the registered fake, so no harness
  mode installs a version. The control makes the value observable and changeable, which makes the
  divergence trace on `getInterfaceVersion()` a branch a test reaches rather than a defensive one
  nothing can drive.
- No validation is applied — any `int32_t` is installed as given — because the point of the
  control is to report a version the snapshot would not.

### FakeHdmiCecService::getController()

- It is the route by which a test configures the controller's canned responses and reads its
  captures; a reference taken before `open()` stays valid afterwards.

### FakeHdmiCecService::getListener()

- Confirming a listener was supplied keeps a silent no-op trigger from being mistaken for a
  delivery failure.

### FakeHdmiCecService::getLastClosedController()

- It proves the adapter kept open and close paired rather than closing something else: a driver
  that closed a null or a stale controller would still receive the canned result, so without this
  capture that mistake is invisible.
- It is consumed by the close-contract cases of the AIDL back-end's L1 suite, which assert it
  equals the exact controller `open()` reported on the successful close, the false-result close and
  the non-ok-status close alike.

### FakeHdmiCecService::getCloseCallCount()

- The adapter's own guard is required to keep a second close from reaching the HAL.

### FakeHdmiCecService::getGetStateCallCount()

- The counter must stay zero across a full open, transmit, receive and close cycle.

### FakeHdmiCecService::getGetPropertyCallCount()

- The counter must stay zero across a full session, since no property has a legacy counterpart.

### FakeHdmiCecService::getRegisterEventListenerCallCount()

- The middleware is the controlling client and never registers a diagnostic listener.

### FakeHdmiCecService::reset()

- The middleware resolves its back-end once per process, so the fake cannot be re-registered per
  case; `reset()` keeps configuration and observations from leaking from one case into the next.

### FakeHdmiCecService::fireOnMessageReceived()

- The caller supplies the exact bytes, so the fake forms no frame of its own. Out of process the
  call crosses the binder driver and the listener runs on the client's binder threadpool, the only
  arrangement in which that threadpool is genuinely exercised.
- `true` means only that a captured listener was invoked. The binder `Status` the invocation
  returned is traced and changes nothing, so `true` is reported even when that call failed, for
  example against a remote listener whose process has died. The three triggers share this meaning.
- What an ok `Status` establishes depends on the listener. A local listener is reached by direct
  virtual dispatch, so the callback ran to completion and the status is the callback's own. A
  remote listener is reached through a proxy and `IHdmiCecEventListener` is declared `oneway`, so
  an ok status means only that the driver accepted the transaction: the remote callback need not
  have run yet, and its outcome is not carried back on this path. Completion against a remote
  listener is established independently, by observing what the middleware did in response — the
  fake's own counters and captures, which the separate host's observation commands read through
  its accessors.
- Fired before `open()`, or after `reset()` cleared the captured listener, the trigger does nothing
  and reports `false`; a test that ignores the return value cannot tell a genuine delivery from a
  no-op.
- The captured listener is copied out before the invocation, so a callback may re-enter the fake
  without deadlocking; a callback that re-enters observes whatever state the fake holds at that
  moment.

### FakeHdmiCecService::fireOnStateChanged()

- It covers a diagnostic callback the adapter must log and act on in no other way: an in-process
  HAL cannot vanish, so there is no legacy counterpart to a state transition, and reacting to one
  would be new behaviour.
- It is called in process by
  `DriverAidlSessionTest.DiagnosticCallbacksAreReportedWithoutDisturbingTheSession`, which reads
  what the adapter logged for it. The separate-process host deliberately exposes no command for
  this trigger, because that assertion is already made in process and an IPC variant would prove
  nothing further.

### FakeHdmiCecService::fireOnMessageSent()

- It covers the second diagnostic callback the adapter must log and act on in no other way:
  synchronous transmit already returns its own status, so nothing about the outcome depends on this
  notification arriving.
- It is called in process by the same case as `fireOnStateChanged()` and exposed by no host command
  for the same reason.

### FakeHdmiCecService::getInstance()

- It is how a test reaches the registered fake to configure it, given that the harness registers
  the fake before initialising the middleware and the test bodies run later.
- It deliberately traces nothing, so that nobody restores a trace on the assumption it was
  overlooked. It is called from the harness, the fixtures and the fake's own paths, so a line per
  call would flood every captured log and dilute the lines a case asserts on, while reporting only
  what `setInstance()` already reported once. A trace behind a quiet log level would be the same
  flood one configuration change away.

### FakeHdmiCecService::setInstance()

- The harness calls it right after it constructs and registers the fake, and with nullptr when it
  tears the fake down. It is not a service registry and not a factory.

### FakeHdmiCecService::mutex

- All critical sections are short, and the listener is copied out before a trigger invokes it so
  no callback runs under the lock. Out of process the interface methods run on binder threads while
  a test thread may be reading a capture or installing a canned response.

### FakeHdmiCecService::interfaceVersionResult

- `setInterfaceVersion()` is the one control that changes it, and `reset()` restores it.

### FakeHdmiCecService::interfaceHashResult

- `setInterfaceHash()` is the one control that changes it, and `reset()` restores it.

### registerFakeHdmiCecService()

- Registration is an explicit callable step rather than something the constructor does, because the
  two callers need different things around it. An in-process harness registers and stops there,
  since a locally registered name resolves to the very object and no transaction crosses the
  driver. The separate host binary needs a thread to serve real transactions, so it starts its
  service-side binder threadpool first, then calls this function, and signals readiness to its
  parent only after this function has reported success.
- That order is load-bearing. Publication makes the name resolvable immediately, so a pool started
  after it leaves a window in which a client can look the name up and transact against a service
  with no thread to serve it — a race the parent can win, which presents as a hung or failed
  transaction rather than as a startup ordering mistake. Folding a threadpool into registration
  would both force one on the in-process caller and fix the order the wrong way round.
- The name comes from `IHdmiCec::serviceName()`, never from a literal, so there is exactly one
  spelling of it in the build and the fake cannot drift from the name the middleware looks up. Only
  the service is published; a client obtains its controller from the out-parameter of `open()`, so
  the controller is never registered separately.
- The pinned C++ service manager exposes no removal API, so a stale registration would decide the
  outcome of a run; the callers establish that nothing is registered before they call, and this
  function does not check it.
- On success the middleware's runtime back-end selection chooses the AIDL path.
- The function's guarantees are narrow. It reports false without touching libbinder at all when the
  supplied service is null, or when the binder driver node is absent or unopenable — the check that
  keeps a host with no kernel binder support from losing its whole run to the fatal driver-open path
  inside the linked libbinder. It reports false when the service manager it obtained is null, and
  when that service manager refuses the name. It does not bound the acquisition of the service
  manager itself: obtaining one retries until binder handle 0 resolves, so a node that opens while
  no service manager is running blocks inside libbinder instead of returning, and the pinned
  library's remaining failure modes are not all reducible to a return value. The bound for those
  cases is the parent harness's readiness timeout, which fails the run when a host does not report
  itself ready in time.

## mocks/hdmicec/fake_hdmi_cec_aidl_service.cpp

### HDMI_CEC_FAKE_AIDL_SERVICE_IMPL (`@defgroup`, Fake Service Implementation Specification)

- Every interface method follows one shape: take the lock, count the call, capture what a test
  needs to read back, trace the call, then answer. The pre-refine wording said the answer is the
  canned response the test installed where one exists, and a fixed value where the declaration
  records that there is deliberately no control.
- Two exceptions qualify that shape. The metadata getters (`getInterfaceVersion()` and
  `getInterfaceHash()` on both classes) count nothing and trace only when the reported value
  differs from the compiled-in one. An allocation poll is recorded only by
  `getTotalSendMessageCallCount()` and `getAllocationPolls()`, never by `getSendMessageCallCount()`
  or `getLastSentMessage()`, and is answered from `setAllocationPollResult()` or
  `setLogicalAddressOccupied()` (default `ACK_STATE_1`, free) rather than from
  `setSendMessageResult()`, while a non-ok `setSendMessageBinderStatus()` fails it like any other
  frame.
- Every setter takes the same lock for one assignment, and every observation accessor takes it to
  return a copy rather than a reference, so a test may install a canned response or read a capture
  while a binder thread is answering and still read a stable snapshot.
- The two static instance functions sit outside that shape and take no lock, for the reason
  recorded on `FakeHdmiCecService::getInstance()`.
- Each member's contract (parameters, return value, pre- and postconditions, and why the member
  exists) is stated once, on its declaration in `fake_hdmi_cec_aidl_service.h`, and copied with
  `@copydoc` rather than restated.
- Superseded: the definitions no longer use `@copydoc`. Each carries its own one-sentence `@brief`
  and the applicable tags, stating what its body does; the declaration remains the fuller contract,
  and any detail a definition comment carried beyond it is kept under the matching symbol below.
- No CEC reasoning, as the pre-refine fake was written: it did not read a frame's destination
  nibble, classify a message as directed or broadcast, inspect an opcode, police a frame length or
  derive a send status from message content. That belongs to the adapter under test; a second copy
  in the fake would make the suite assert the fake's opinion instead of the adapter's behaviour.
  The fake's purpose is to make each adapter branch reachable.
- Superseded: the "no CEC reasoning / never reads the destination nibble" statement no longer holds
  once the fake answers the adapter's logical-address allocation polls. A self-addressed one-byte
  poll is answered as not acknowledged unless a test marks that address occupied, and
  `getLogicalAddresses` by default reflects the addresses registered through the fake's controller.
  The only CEC reasoning the fake now carries is recognising an allocation poll (a one-byte frame
  whose initiator equals its destination) and validating logical-address add and remove requests
  as the `IHdmiCecController` contract does.
- No GoogleMock and no GoogleTest, although the legacy driver double in the same directory uses
  both: this translation unit is also compiled into the separate fake-service host binary, which
  links only the AIDL stub and binder libraries, so a single reference to either framework would
  break that link.
- No binder threadpool: the service-side threadpool belongs to the host binary and the client-side
  one to the middleware adapter. Starting one here would blur the in-process and out-of-process
  cases, and telling those two apart is the reason both exist.

### `@file`

- No `.aidl` is authored here, no interface is added and no method is added to an interface.
- Test scope: the file is built only for test targets (the fake-service host binary and the test
  runners), so no symbol defined here can reach the shipped middleware library.

### FAKE_HDMI_CEC_BINDER_DRIVER

- The linked libbinder aborts the whole process when it cannot open its driver. On a host without
  kernel binder support, checking the node first turns "the process died during test set-up" into
  "registration reported false", which is the behaviour `registerFakeHdmiCecService()` documents.
- `/dev/binder` is the default node name the SDK's own process state uses, and the node a test
  runner and its service manager share.

### FAKE_HDMI_CEC_TRACE_ABSENT

- Spelled once so no trace site drifts into printing an empty field, where a reader could not tell
  a missing object from a missing value.

### fakeHdmiCecTraceLabel

- The registry and its sequence are function-local rather than file-scope so construction is
  ordered by first use, not link order: the translation unit is compiled into two binaries (the L1
  test runner and the fake-service host) and the first caller differs in each.
- The lock is the registry's own and is taken nowhere else, so a call from inside one of the fake's
  critical sections (where most trace sites sit) cannot deadlock against it, and a call on a binder
  thread cannot race one from a test thread.
- Nothing is pruned: reusing an ordinal would let two different objects appear under one label in a
  single capture, the confusion the ordinal exists to remove. The registry is bounded by the
  handful of objects one run traces.

### FakeHdmiCecController::addLogicalAddresses

- The vector is captured as it arrived, neither normalised, sorted, deduplicated nor trimmed,
  because its width is what the single-element assertions read.
- The optional delay is read under the instance mutex, and the mutex is dropped before the sleep,
  so a concurrent capture read on a remote fake answering on binder threads is never blocked behind
  it. Zero is the default and costs one lock acquisition and a comparison, which is why the delay is
  unconditional rather than compiled out. `setAddLogicalAddressesDelayMs()` records why a real
  sleep is the only way to reach the middleware's slow-call threshold.
- After the delay, the call counter and the capture advance before the canned binder status is
  examined, so a failing arm is still counted and captured.
- A null out-parameter is traced rather than dereferenced; a true answer with an ok status still
  registers the addresses.

### FakeHdmiCecController add/remove contract validation

- With no forced result, `addLogicalAddresses()` and `removeLogicalAddresses()` check every
  address of the request against `FAKE_HDMI_CEC_MIN_DIRECT_ADDRESS`..`FAKE_HDMI_CEC_MAX_DIRECT_ADDRESS`
  (0..14, the directly addressable range) and against the registrations before changing anything.
  One refused address refuses the whole request, so a mixed request such as `{ 6, 4 }` with 4
  held registers nothing.
- Without this, a duplicate add or a removal of an absent address reported `true` while the
  registry silently changed nothing, so an adapter mistake a conforming HAL refuses would pass.
- An empty request is accepted and changes nothing. Duplicates inside one request are checked
  only against the registrations held before the call, and are registered once.
- A forced `true` keeps the pre-validation semantics: the add registers every address not yet
  registered and the removal erases every address given. A forced `false` changes nothing.
- The trace line keeps its format and prints the computed result, so a refused request shows as
  `reporting false` with an ok status.

### FakeHdmiCecController::removeLogicalAddresses

- A false result and a non-ok status are ordinary outcomes on the removal path rather than errors,
  so neither is treated differently in the body from the successful case.
- Superseded: "treated no differently from the successful case" no longer holds once the controller
  keeps a registration record. Only an ok status with a `true` result removes the addresses from
  `getRegisteredLogicalAddresses()`; a false result or a non-ok status leaves the registrations as
  they were. Unlike `addLogicalAddresses()`, the body takes no configurable delay.
- Otherwise it is identical in shape to `addLogicalAddresses()`: the counter and the capture advance
  before the canned status is examined, and a null out-parameter is traced rather than dereferenced.

### FakeHdmiCecController::sendMessage

- Pre-refine: the frame was captured whole and never examined. Nothing read a destination nibble,
  inspected an opcode or applied a length limit, so the fake could not classify a frame as directed
  or broadcast, and the meaning of `ACK_STATE_0` and `ACK_STATE_1` (inverted between those two
  cases) stayed a property of the adapter under test.
- Superseded: the fake now answers the adapter's self-addressed allocation polls (not acknowledged
  unless a test marks the address occupied), so "never examined" no longer describes the body.
- The total counter advances first on every call. A poll is recorded before the canned binder
  status is examined, and a non-ok status is returned before the poll's answer is written, so a
  failed poll is still visible in `getAllocationPolls()`.
- A non-ok canned binder status therefore fails an allocation poll exactly as it fails any other
  frame. Otherwise a poll is answered with the status installed for its address through
  `setAllocationPollResult()` or `setLogicalAddressOccupied()`, `ACK_STATE_1` (free) by default; it
  never advances `getSendMessageCallCount()`, never replaces `getLastSentMessage()` and never reads
  `setSendMessageResult()`.
- Any other frame advances `getSendMessageCallCount()`, is captured whole with no length limit and
  is answered with the canned send status. On either path a null out-parameter is traced rather
  than dereferenced.

### FakeHdmiCecController::getInterfaceVersion and getInterfaceHash

- The divergence trace fires only when the reported value differs from the compiled-in one, so the
  line that does appear names the moment the controller was made to report something the snapshot
  would not, rather than leaving a silent metadata change to be inferred from a later failure.
- `setInterfaceVersion()` / `setInterfaceHash()` install such values, and the
  `DriverAidlCompatibilityTest` cases that drive them are what make these branches reached.
- Superseded: "inferred from a later failure" overstates the effect. No selection outcome reads the
  controller's metadata (the adapter's compatibility check reads only the `IHdmiCec` service's), so
  a divergent controller value causes no later failure; the trace, and the cases that assert on it,
  are its only observers. Neither getter counts the call.

### FakeHdmiCecController::setAddLogicalAddressesDelayMs

- A negative value is stored as given and treated as "no delay" by the comparison at the point of
  use, which keeps the setter free of a clamp a caller would have to reason about.
- The setter stores the value only; `addLogicalAddresses()` reads it under the lock and sleeps with
  the lock dropped.

### FakeHdmiCecController::setRemoveLogicalAddressesResult

- Held in a member of its own, so installing it disturbs no other canned response.

### FakeHdmiCecController::setSendMessageResult

- The value is stored without interpretation, so no reading of `ACK_STATE_0` or `ACK_STATE_1`
  (whose sense inverts between directed and broadcast frames) is fixed in the fake. It never
  answers an allocation poll.

### FakeHdmiCecController binder-status setters

- `setAddLogicalAddressesBinderStatus()`, `setRemoveLogicalAddressesBinderStatus()` and
  `setSendMessageBinderStatus()` each fill a member of their own, so failing one of the three
  controller methods leaves the other two alone.
- The send binder status is held independently of the canned send status, so the two can be
  installed in any combination; a non-ok one fails allocation polls as well as other frames.

### FakeHdmiCecController::setLogicalAddressOccupied and setAllocationPollResult

- Both write one per-address answer map. `setLogicalAddressOccupied(address, true)` installs
  `ACK_STATE_0` (taken), `setAllocationPollResult()` installs any status and replaces an earlier
  answer, and `setLogicalAddressOccupied(address, false)` erases the address's entry, whichever
  setter installed it, restoring the `ACK_STATE_1` (free) default. `reset()` clears the map.

### FakeHdmiCecController::setInterfaceHash and setInterfaceVersion

- The hash setter traces in the same shape as the service's setter, so one line records where in a
  run the controller was made to report a divergent hash.
- No validation: the caller owns which hash is reported, and any `int32_t` is a version the caller
  may want reported.
- Installing `IHdmiCecController::HASHVALUE` or `IHdmiCecController::VERSION` restores the
  default, after which the matching getter traces nothing.

### FakeHdmiCecController::reset

- At the pre-refine member set, one critical section restores all three canned results, all three
  canned statuses and both metadata values, and clears all three captures and all three counters.
- Spelling each default beside the line that restores it keeps a documented default and the code
  that reinstates it together. Restoring the metadata pair stops a case that installed a divergent
  hash or version from deciding the outcome of the next one.
- At the current member set the same critical section also clears both forced add and remove
  results (so requests are validated again), the total send counter, the allocation-poll answers
  and records, and the registrations, so no case observes a half-restored controller.

### FakeHdmiCecService::~FakeHdmiCecService

- The instance pointer is compared before it is cleared, so a fake destroyed after a second one was
  published leaves that second one reachable.

### FakeHdmiCecService::getState and getProperty

- `getState`: there is no member behind `DEFAULT_STATE`, so nothing can make the method report
  anything else, and no canned status exists to make it fail.
- `getProperty`: no `PropertyValue` is constructed anywhere in the body, so there is no fabricated
  metric for a test to assert against itself.

### FakeHdmiCecService::getLogicalAddresses

- Each width of the answer (empty, one entry, more than one) reaches a different arm of the adapter
  under test, which is why the vector is never normalised, sorted, deduplicated or truncated.
- Superseded: the pre-refine comment said the canned vector is copied out. By default the fake now
  reflects the addresses registered through its controller; canned overrides remain possible.

### FakeHdmiCecService::open

- An ok status carrying nothing usable (a null controller) is the one combination a test has to
  ask for explicitly, because the null-controller flag is read only on the ok arm.
- The listener is captured before the canned status is examined, so the receive path stays
  exercisable against a rejected session.

### FakeHdmiCecService::close

- "A callback arriving during or after a close is rejected by the adapter's own state guard" is a
  required behaviour; clearing the captured listener in `close` would make it untestable.
- Only an ok status with a true result drops the owned controller's registrations, as a successful
  `IHdmiCec` close does; a false result or a non-ok status leaves them in place.

### FakeHdmiCecService::registerEventListener and unregisterEventListener

- `registerEventListener()` stores the offered listener nowhere, so no second delivery route exists
  for a test to reach by accident; `unregisterEventListener()` therefore has nothing to withdraw.

### FakeHdmiCecService::getInterfaceVersion and getInterfaceHash

- The trace fires only on divergence, so an ordinary compatible run prints nothing here and the
  line that does appear names the change at the moment it would matter.
- Superseded: the former comment said the hash trace marks "the deliberately incompatible run". A
  divergent hash is overridden metadata, not necessarily an incompatible service.
  `halcompat::isCompatible()` refuses a null service, an empty or `"-1"` hash and, unless unfrozen
  servers are allowed (the adapter does not allow them), `"notfrozen"`; any other hash passes and the
  verdict then rests on the version alone, because no frozen hash is compared for equality.
- Both values take effect under local (in-process) dispatch only; across a binder transaction the
  generated `onTransact()` answers from the compiled-in constants.

### FakeHdmiCecService::setLogicalAddressesResult

- Nothing rejects an empty vector, caps its width or validates an entry, because every one of
  those shapes is a case a test needs to be able to install.

### FakeHdmiCecService::setCloseResult and setCloseBinderStatus

- The close result and the close binder status are held in separate members, so a test can drive a
  transport failure and a HAL-reported refusal independently of one another.

### FakeHdmiCecService::setOpenReturnsNullController

- Only a flag is stored; the owned controller is neither released nor replaced, so clearing the
  flag restores the ordinary successful open with the same controller object as before.

### FakeHdmiCecService::setOpenBinderStatus

- `EX_ILLEGAL_STATE` is the exception the interface documents for an already-open service.
- The status is stored uninterpreted, so any exception code reaches the adapter as constructed.

### FakeHdmiCecService::setGetLogicalAddressesBinderStatus

- The failed query and the successful but empty query can be installed separately even though the
  adapter reports the same outcome for both.

### FakeHdmiCecService::setInterfaceHash and setInterfaceVersion

- The one line a run prints from the hash setter is the record of where the fake was made
  deliberately incompatible; the harness owns which value produces the refusal.
- Superseded: the line records where the reported hash was overridden, and whether the override is
  refused depends on the value installed. The L1 harness's incompatible mode installs `"-1"`, which
  `halcompat::isCompatible()` refuses; installing `IHdmiCec::HASHVALUE` restores the default, and
  any value other than an empty string, `"-1"` or `"notfrozen"` passes the hash check.
- The version is held in a member of its own, so installing it disturbs neither the hash nor any
  canned response.

### FakeHdmiCecService::getController

- The strong pointer is copied out so the controller outlives the call whatever the service does
  next. No statement in the class reassigns the member, which is what makes the never-null
  guarantee hold for the life of the service.

### FakeHdmiCecService::getListener and getLastClosedController

- Both copy a strong pointer out under the lock, so the object a caller obtained cannot be released
  underneath it by a concurrent `reset()`.
- `getLastClosedController()` reports whatever `close()` was handed, a null included, because "the
  adapter closed nothing usable" is itself a result a case has to be able to observe.

### FakeHdmiCecService::reset

- At the pre-refine member set, one critical section restores every canned response and both
  metadata values, clears both captures and zeroes all seven counters.
- Each default is spelled beside the line that restores it, keeping the restored value and the
  documented default from drifting apart.
- Three statuses, not seven: `getState()`, `getProperty()`, `registerEventListener()` and
  `unregisterEventListener()` answer a fixed ok because the middleware never calls them, and a
  settable failure arm on a method nothing under test reaches would imply coverage that does not
  exist. Their counters are still cleared, because "the adapter never called this" is asserted
  against them.
- The single critical section means no case observes a half-reset fake or inherits a divergent hash
  or version. The owned controller is left alone, as the declaration's warning explains; a case
  that configured it resets it through `getController()->reset()`.

### FakeHdmiCecService::fireOnMessageReceived, fireOnStateChanged and fireOnMessageSent

- Copying the listener out inside the critical section and invoking it outside is what makes the
  re-entrancy the declaration describes safe.
- The traced status follows the declaration's local-versus-remote distinction: from a local
  listener it is the callback's own return value; from a remote proxy the interface is oneway and
  the value reports only that the driver accepted the transaction. Nothing waits for, or can
  observe, a remote callback's own outcome.
- `fireOnStateChanged` matches `fireOnMessageReceived` in where the lock is released and in what
  the traced status does and does not establish.
- Each trigger returns true whenever a captured listener was invoked, whatever `Status` the
  invocation returned; that status is traced and conditions nothing.
- `fireOnMessageSent` has the same shape; its invocation status is named `listenerStatus` because
  `status` already names the send status being notified.

### FakeHdmiCecService::getInstance and setInstance

- `getInstance` needs no lock because no test thread and no binder thread can be running while the
  value changes.
- Superseded: neither harness establishes that quiescence, and only one clears the pointer. Neither
  `getInstance()` nor `setInstance()` takes a lock; what the callers rely on is the order of the
  writes. In the L1 harness, `publishFakeForMode()`, reached from `CecTestEnvironment::SetUp()`
  before `LibCCEC::init()` and so before any test body, registers the fake, then calls
  `setInstance()` and keeps a strong reference for the process; `CecTestEnvironment::TearDown()`
  never clears the pointer. The separate-process host's `main()` calls `setInstance()` before it
  starts its service-side binder threadpool and registers the fake, and calls
  `setInstance(nullptr)` once serving ends, without stopping that threadpool; its early-return
  failure paths do not clear it. Within this tree only the L1 `DriverAidlSessionFixture::SetUp()`,
  on the test thread, reads the pointer; the host's binder threads answer through the fake object
  itself and never call `getInstance()`.
- `setInstance` traces the label of the object it replaces and then the label now in place, so a
  log carries one record of every change to the pointer, which is why `getInstance()` can stay
  silent.

### registerFakeHdmiCecService

- Four checks, in order, and the order bounds the damage: the null service and the binder driver
  node are tested before libbinder is touched, because the linked libbinder treats a driver it
  cannot open as fatal; a host with no kernel binder support therefore gets a false return instead
  of losing its whole run, every legacy case included. Only then is a service manager obtained and
  tested, and only then is the name offered and the resulting status tested.
- A driver present but speaking a protocol version the linked libbinder was not built for stays
  fatal inside libbinder and is deliberately not screened here: policing that is the middleware's
  own preflight, and a second copy of a production decision inside a test fake would be one more
  thing to keep in step.
- Nothing in the body time-limits the service manager request, which is why the declaration hands
  that bound to the parent harness.
- A false return is always accompanied by a trace giving its reason, and the successful publication
  is traced too.

## mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp

Detail moved out of the source comments of the separate-process fake-service host. The source comments say what each symbol does; this section keeps the full contracts and the reasoning behind them.

- Superseded statements: none. No original comment in this file describes logical-address or physical-address behaviour, so the one-address-per-device and fixed-1.0.0.0 changes leave all of it accurate.

### HDMI_CEC_FAKE_AIDL_SERVICE_HOST (group block)

- The file is a whole program: a `main()` that publishes the test-scope fake `com.rdk.hal.hdmicec` service in a process of its own, serves transactions against it, and runs until its parent asks it to stop.
- Process boundaries are not a binder implementation detail; they are the thing under test. libbinder resolves a service name registered in the calling process to the local `BBinder`, so `interface_cast` returns that same object: no `Bp*` proxy is created, no transaction crosses the binder driver, and the client's threadpool is never involved. A fake registered inside the test runner therefore cannot prove the transport, however faithfully it implements the interface. Hosting the same fake in a separate process makes the middleware hold a real proxy and receive its event callbacks on a binder thread.
- The file deliberately adds nothing else: no wrapper, no interface and no abstraction of its own, and its only types are two file-local helpers, the `ReplyOutcome` enum and the `TransactionMethod` row of the `calls` method tables. The only interface published is the one the `.aidl` files define, and the fake already knows how to publish itself under the production service name. The program is a startup order, a readiness signal, a line-oriented control and observation channel over two inherited descriptors, and a shutdown wait.
- The channel exists because a separate process is opaque. Once the fake lives in the host, the runner can no longer call `fireOnMessageReceived()` to stimulate the receive path or read `getLastSentMessage()` to see the last application frame the middleware transmitted. Without a channel the out-of-process invocation could only assert that a call did not throw, which a no-op or a corrupt transmit passes as well as a correct one.
- The channel is a plain pipe rather than a second binder interface so that it stays trustworthy when the binder path under test is broken, and so that no new AIDL surface is invented to test the existing one.

### File block (`@file`)

#### Role

- This binary is the only place the AIDL receive path is genuinely exercised. It is the counterpart of the out-of-process test invocation, in which the middleware resolves a real proxy over the binder driver and its listener callback arrives on a binder threadpool thread rather than on the caller's own stack. Without this program that invocation cannot exist.
- The text below is the normative statement of the host side of the lifecycle contract and of the control protocol. The parent end lives in the L2 runner's harness (`tests/L2Tests/test_main.cpp`) and is written against it; nothing here may change without changing that client.

#### Startup order (in this order and no other)

1. Install the shutdown handlers, so a termination signal arriving at any later point still produces a clean exit rather than a default-disposition kill.
2. Resolve the readiness file descriptor. Only the spelling of the value is checked here (a plain non-negative descriptor number), and it is checked before anything is published, so a botched handoff costs an exit code and no publication. Whether the descriptor is open and writable is settled by the readiness write in step 7, which fails with `EXIT_READINESS_WRITE_FAILED`.
3. Resolve the control and observation channel, if the parent supplied one. Both descriptors are checked for spelling and for being open in this process in the direction they will be used, because a channel this program cannot serve must be refused before anything is published rather than discovered as an `EBADF` three commands later. Only when a channel was supplied, ignore `SIGPIPE` so that a client closing its end reports `EPIPE` instead of terminating this process.
4. Confirm a binder driver node is present and openable.
5. Confirm nothing is already published under the production service name. Anything there is a hard failure, never a condition to work around: the run's outcome would otherwise depend on a process this suite does not own.
6. Construct the fake, start the service-side threadpool, then publish the fake. The pool starts before publication so no transaction can arrive with no thread to serve it.
7. Write the readiness line, only now, after publication has succeeded and the pool is running, so a parent that has seen the line may rely on the service being both published and served.
8. Serve the control and observation channel, if supplied, until a `shutdown` command, end of file on the control descriptor, a parent that has closed the observation descriptor, or a termination signal; each of the four is a clean end of session. Without a channel, block until signalled, exactly as the program always has.
9. Return `EXIT_SUCCESS`.

#### Environment

- `CEC_FAKE_AIDL_HOST_PATH` is where the parent finds this binary. The build sets it and the L2 harness reads it; this program never does, being already running by the time it matters.
- `CEC_FAKE_HOST_READY_FD` is the number of an inherited descriptor, the write end of a pipe the parent holds open, on which the readiness line is delivered. Unset means "no parent is listening", and the line goes to standard output so a human can run the binary from a shell and watch it work. Set but not a plain non-negative descriptor number is a hard failure, because a parent that asked to be signalled on a pipe and was silently answered on standard output would wait out its whole timeout with nothing to explain why. That check is on the value only; whether the descriptor is open and writable is settled by the readiness write, which exits `EXIT_READINESS_WRITE_FAILED` when it is not.
- `CEC_FAKE_HOST_CONTROL_FD` is the number of an inherited descriptor, the read end of a pipe the parent writes to, from which newline-terminated ASCII commands are read.
- `CEC_FAKE_HOST_OBSERVE_FD` is the number of an inherited descriptor, the write end of a second pipe the parent reads from, to which exactly one newline-terminated reply is written per command.
- Both channel variables are optional and travel together. Both unset means no channel: the program then behaves exactly as it did before the channel existed, so an existing invocation that supplies neither is unaffected. One set without the other, or either set to something that is not a usable descriptor of the right direction, is a hard failure that exits `EXIT_BAD_CONTROL_CHANNEL` and writes no readiness token: a parent that asked for a channel and was silently served without one would sit in its bounded wait for a reply that can never come.
- Unlike the readiness descriptor, the two channel descriptors are established as open and correctly directed before they are accepted, because this program reads and writes them itself throughout the session.

#### Control and observation protocol

- Framing: the parent writes one command per line, terminated by a single `\n`; the host writes exactly one reply line per command, terminated by a single `\n`. A trailing `\r` on a command line is tolerated and stripped. Tokens are separated by runs of spaces or tabs. Every reply begins with `OK ` or `ERR `, so a client can classify an outcome before parsing it. A blank or whitespace-only line is not a command: it is ignored and produces no reply.
- A command line longer than `MAX_COMMAND_LINE_LENGTH` bytes is answered `ERR command-too-long` and discarded unparsed, whether or not its terminator arrived in the same read; how a client's bytes happened to be split must never decide whether its command was accepted.
- Reply delivery: each reply is written within one whole-call deadline of `OBSERVE_WRITE_TIMEOUT_MS` on the monotonic clock and is at most `MAX_REPLY_LINE_LENGTH` bytes, the size up to which a pipe write is atomic, so a reply is never delivered in pieces. A client that stops reading costs one deadline and then the session: the reply is abandoned and the host exits `EXIT_CONTROL_CHANNEL_FAILED`, the one outcome a client can act on where a hang is not. A reply that would exceed the cap (only the `ERR unknown-command <verb>` echo of a verb from a client that has lost its framing can) is refused unwritten and ends the session the same way.
- Commands, with every reply each can produce:
  - `ping`: liveness, touching nothing. Replies `OK pong`.
  - `deliver <lowercase-hex>`: invokes `onMessageReceived` on the listener the middleware handed to `open()`, with exactly the bytes the hex encodes (`deliver 0f8f` delivers two bytes). Replies `OK delivered <byteCount>`, `ERR no-listener` when no listener is held, or `ERR bad-hex` when the payload is not an even-length run of hexadecimal digits. `OK delivered` states only that the callback was invoked on the held listener: the callback is `oneway`, so whether the middleware queued, decoded and dispatched the frame is what the test asserts on its own side of the boundary.
  - `sent-count`: replies `OK sent-count <n>`, the fake controller's application-frame count, `getSendMessageCallCount()`: every `sendMessage()` call except allocation polls (one-byte frames whose initiator equals their destination), which the fake records separately in `getAllocationPolls()`. No verb reports the poll record or `getTotalSendMessageCallCount()`, the count of every call.
  - `last-sent`: replies `OK last-sent <lowercase-hex>`, the bytes of the last application frame the fake controller captured, `getLastSentMessage()`; an allocation poll never replaces it. The hex field is empty when nothing has been captured, so the reply is `OK last-sent` followed by one space and then the newline.
  - `open-count`: replies `OK open-count <n>`, the fake service's real `open()` invocation count.
  - `close-count`: replies `OK close-count <n>`, the fake service's real `close()` invocation count. `open-count` and `close-count` are consumed by `DualPathAidlFlowTest` in `tests/L2Tests/ccec/test_DualPathIntegration.cpp`, which asserts the live-session invariant `open - close == 1` and then exact per-transition deltas around the one close/reopen cycle that tier performs. Only an out-of-process fake can give that evidence: it shows the session lifecycle really crossed the driver, and how many times, which an in-process fake answering from the same address space cannot.
  - `listener`: replies `OK listener present` or `OK listener absent`.
  - `registered`: replies `OK registered <decimal,...>`, the logical addresses registered through the fake controller in registration order; the field is empty when none is registered.
  - `calls`: takes no argument and replies on one line, single-spaced, fields in this fixed order: `OK calls IHdmiCec.getState=<n> IHdmiCec.getProperty=<n> IHdmiCec.getLogicalAddresses=<n> IHdmiCec.open=<n> IHdmiCec.close=<n> IHdmiCec.registerEventListener=<n> IHdmiCec.unregisterEventListener=<n> IHdmiCec.getInterfaceVersion=<n> IHdmiCec.getInterfaceHash=<n> IHdmiCec.other=<n> IHdmiCecController.addLogicalAddresses=<n> IHdmiCecController.removeLogicalAddresses=<n> IHdmiCecController.sendMessage=<n> IHdmiCecController.getInterfaceVersion=<n> IHdmiCecController.getInterfaceHash=<n> IHdmiCecController.other=<n>`. Each count is the number of binder transactions with that method's generated `TRANSACTION_*` code that the fake service or its controller received in its `onTransact()` since construction; `other` sums every code without a named field. Counting at the transport sees what the fake's own counters cannot: remote metadata calls, which the generated code answers from constants, and allocation polls, which `sent-count` excludes, so `IHdmiCecController.sendMessage` counts every transmit. `PING_TRANSACTION`, `EXTENSION_TRANSACTION`, `DEBUG_PID_TRANSACTION` and `SET_RPC_CLIENT_TRANSACTION` are answered by `BBinder::transact()` before `onTransact()` and are never counted; none is an AIDL method. `ERR bad-args calls` answers an argument, and `ERR no-controller` a fake holding no controller. The reply is a few hundred bytes, well under `MAX_REPLY_LINE_LENGTH`.
  - `shutdown`: replies `OK shutdown`, then performs the same clean teardown as the signal path and exits `EXIT_SUCCESS`. Anything queued behind it is not served, so `shutdown` is by definition the last command of a session.
  - Anything else: replies `ERR unknown-command <verb>` and the loop continues, so one mistyped command does not end the session and a client whose vocabulary does not match is told so rather than silently served.
- Three verbs are deliberately absent, recorded so nobody restores one on the assumption it was overlooked:
  - `state-changed` and `message-sent`, which would fire the fake's two diagnostic callbacks over IPC. The adapter must log those callbacks and act on them in no other way, and that is already asserted in process by `DriverAidlSessionTest.DiagnosticCallbacksAreReportedWithoutDisturbingTheSession`, which calls `FakeHdmiCecService::fireOnStateChanged()` and `fireOnMessageSent()` directly and reads what they logged; an out-of-process variant would add a verb without adding evidence.
  - `reset`, which cannot be made safe: `FakeHdmiCecService::reset()` clears the captured listener, so a mid-session reset would silently destroy the live session's receive path and every later `deliver` would answer `ERR no-listener` for a reason no assertion would explain. The out-of-process invocation opens its session once, in the harness, and reads counters rather than rewinding them.
- Two further error replies apply to every command that takes arguments: `ERR bad-args <verb>` when the argument count is wrong, and `ERR no-controller` if the fake service ever handed out no controller. The latter is documented so a client can classify every line the host can emit, not because it is reachable today.

#### Parent's obligations

- Channel: the parent creates two pipes, passes the control read end and the observation write end to the child as inherited descriptors, and names those numbers in `CEC_FAKE_HOST_CONTROL_FD` and `CEC_FAKE_HOST_OBSERVE_FD`. Descriptors created with `O_CLOEXEC` (which is how they should be created, so no unrelated exec leaks them) do not survive the exec unless the parent clears `FD_CLOEXEC` on the child's copies between `fork()` and `exec()`, as it already does for the readiness descriptor. A parent that forgets names descriptors the host rejects as unusable, which is the intended failure: rejected loudly beats served silently.
- The parent keeps its two ends open for as long as it intends to drive the host, reads each reply before issuing the next command, and bounds every wait for a reply. Closing the control write end is a legitimate way to end the session: the host reads end of file and shuts down cleanly, as for a signal.
- Readiness: the parent creates the pipe, passes its write end as an inherited descriptor, names it in `CEC_FAKE_HOST_READY_FD`, and waits for the token under a bounded timeout after which it fails the run rather than proceeding. The bound is not optional: a run that proceeds without the token tests the legacy back-end while reporting an AIDL result. In teardown the parent terminates and reaps the child, so no host outlives the suite that launched it.

#### Readiness

- The signal is the exact line `FAKE_HDMI_CEC_AIDL_HOST_READY\n`, written once with a raw write that loops over partial writes and retries an interrupted one. It is a fixed token matched verbatim. Everything else the program prints is diagnostics on standard output and no part of the signal.

#### Warnings and note from the file block

- `CEC_TEST_AIDL_MODE` is not read here. It is read by the L1 and L2 harnesses and nowhere else; this host is launched by the remote mode rather than told about it, and a second reader would be a second place for the modes to drift.
- A sleep-based readiness signal is not acceptable anywhere in this contract. A timed wait in place of the token converts a race into a flake: it passes on a fast machine, fails on a loaded one, and never proves the service was published.
- The host does not reimplement the middleware's bounded binder preflight. That predicate is production code in the middleware's own source directory and this binary links no libRCEC, so its protocol-version equality check and its bounded wait for binder handle 0 are both absent. Consequence: where a driver node exists but no service manager is running, reaching the service manager blocks (it retries at one-second intervals until handle 0 resolves) and the host sits there. The parent's bounded readiness timeout is the only guard against that case, which is part of why it is mandatory. A service manager is an unconditional runtime prerequisite wherever a binder driver is present.
- The threadpool started here is the service side, serving transactions addressed to the fake. It is unrelated to the client-side pool, which the middleware's AIDL back-end starts inside its own `open()`. The fake implementation starts neither, which keeps the in-process and out-of-process cases distinguishable.
- Test scope only: the program is a `noinst_PROGRAMS` target built for test targets exclusively; it is never installed and no production source list references its directory, so nothing here can reach the shipped middleware library.

### renderUntrustedValue (and the untrusted-value rendering contract)

- Defect removed (CWE-117): diagnostics in this file used to stream caller-supplied text verbatim. A value carrying a newline did not stay inside a message; it ended the message and began a line of its own, and a line beginning `::error::` is a GitHub Actions workflow command. Measured before the fix: `CEC_TEST_AIDL_MODE=$'bogus\n::error::FORGED_L1_ANNOTATION'` produced a standalone forged `::error::` annotation in the run log, and a five-thousand-character value produced more than ten kilobytes of diagnostics, burying the real failure.
- The contract, in the order the steps are applied, because the order makes the escaping unambiguous and reversible:
  - (a) A literal backslash becomes `\\` first, so every escape introduced below is distinguishable from the same characters occurring literally in the value. Putting the backslash arm first is the whole of clause (a): a rendered `\n` is then unambiguously either the two characters the value contained or a newline it contained, never either.
  - (b) `0x0A` becomes `\n`, `0x0D` becomes `\r`, `0x09` becomes `\t`, and every other byte outside printable ASCII `0x20..0x7E` becomes `\xNN` in lower-case hex. Classification is by byte value on `unsigned char` and calls no locale-sensitive function (no `isprint`, no `iswprint`, no ctype table), so it behaves as under `LC_ALL=C` whatever the locale, and no multi-byte sequence can hide a control character.
  - (c) The rendering is truncated at `RENDER_LIMIT` characters and `...[truncated, N bytes total]` is appended when it truncates, `N` being the value's own length in bytes. A caller cannot drown a log, and the message still says how much was withheld.
  - (d) It is applied to the value, never to the surrounding message, and the result is always delimited with double quotes, so an empty value shows as `""` rather than as a gap in a sentence.
  - (e) It never begins a diagnostic line: every message keeps its own prefix in front of it, and the rendering itself begins with `"`. With (b), which leaves no raw newline in the output, that makes a forged standalone `::error::`, `::warning::` or `::notice::` line unreachable by construction rather than unlikely.
- Every diagnostic in this file that names a value supplied by the environment, the command line or another process goes through it. Any input is acceptable, including embedded newlines, carriage returns, ANSI escape sequences, invalid UTF-8 and multi-kilobyte payloads.
- The result is always quoted, always one line, never longer than `RENDER_LIMIT` characters plus the truncation note and the two quotes, and contains no byte below `0x20` or above `0x7E`, so it cannot terminate the line it sits on or begin a new one.
- Applying it to a whole message would escape the message's own punctuation and destroy the prefix that clause (e) depends on.
- This is one of five copies of one contract; the five must not diverge, and a change to any is a change to all:
  - `tests/L1Tests/run_coverage.sh`: `render_untrusted()`
  - `.github/workflows/aidl-path-tests-rootfs.sh`: `render_untrusted()`
  - `tests/L1Tests/test_main.cpp`: `renderUntrustedValue()`
  - `tests/L2Tests/test_main.cpp`: `renderUntrustedValue()`
  - `mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp`: `renderUntrustedValue()`
- The two convenience overloads are `inline` so a copy that does not need one of them in its own file raises no `-Wunused-function` warning: the copies are identical by construction, and which overloads a file calls is a property of its callers.
- They are five file-local copies rather than one shared helper by choice: two are shell and three are C++, they live in three build targets and one non-built script, and a shared header would add a build-system edge for a twenty-line function. The cost is the cross-reference, which is why every copy carries it.
- C-string overload: a null pointer renders as the four characters `<unset>`, undelimited, because "unset" and "set to the empty string" are different facts and a diagnostic showing both as `""` would describe the wrong one.

### Diagnostic and channel constants

- `TRACE_PREFIX`: a failed out-of-process invocation is diagnosed from these lines and nothing else. The parent sees a timeout, and only this output says which step the host reached before it stopped. The prefix separates these lines from the fake's own `[FakeRegistration]` and `[Fake...]` lines in an interleaved capture.
- `READINESS_TOKEN`: the parent matches it verbatim, so it is a fixed contract and not free to vary. The trailing newline is part of it because a parent reading a line needs the terminator to know the line is whole.
- `CONTROL_FD_VARIABLE`: the read end of a pipe the parent writes to. Unset together with `OBSERVE_FD_VARIABLE` means no channel was supplied and none is served; set without its partner, or set to something unusable, is a hard failure.
- `OBSERVE_FD_VARIABLE`: the write end of a second pipe the parent reads from. Exactly one reply line is written per command received and nothing else ever, since every diagnostic goes to standard output, so a client parsing this descriptor sees replies alone.
- `MAX_COMMAND_LINE_LENGTH`: the control descriptor is a pipe an authorised parent owns, not untrusted input, but an unbounded line buffer is still an unbounded allocation driven from outside the process. The cap is far larger than any command in the vocabulary (the longest realistic one is a `deliver` carrying a maximum-length CEC frame, well under a hundred characters), so only a client that has lost its framing can reach it, which is exactly the case it exists to answer.
- `CONTROL_READ_CHUNK`: sized to swallow a whole command, usually several, in one call. A short read is ordinary and costs nothing: the reader buffers a partial line and resumes on the next poll.
- `TRACED_COMMAND_LIMIT` (removed constant): it bounded the traced command text at 120 bytes with a plain `substr()`. It was removed because `renderUntrustedValue()` now bounds that trace at `RENDER_LIMIT` and escapes as well as bounds; the `substr()` did not escape, so a command carrying a carriage return reached the log intact. Two bounding mechanisms with different guarantees is exactly the divergence the one contract exists to prevent. The command itself is dispatched whole regardless of the trace bound.
- `OBSERVE_WRITE_TIMEOUT_MS`: a pipe whose reader has stopped reading eventually stops accepting writes, and a blocking write there would hang the program with no diagnostic and no exit code; that presents as a hung suite rather than a failed one. Every reply write is therefore bounded: the host would rather exit `EXIT_CONTROL_CHANNEL_FAILED` and let the parent's own bounded wait fail loudly than block. The bound is generous by design, since a healthy parent reads each reply before sending the next command. It is a whole-call budget, not a per-attempt one: `writeReplyLine()` converts it once into an absolute `CLOCK_MONOTONIC` deadline and derives every wait from the time left, so a reply interrupted repeatedly, or accepted in stages, cannot cost more than this value in total.
- `MAX_REPLY_LINE_LENGTH`: `PIPE_BUF` is the size up to which a pipe write is atomic, which is the whole reason for the cap. On a descriptor in nonblocking mode, a write of at most `PIPE_BUF` bytes either transfers the line whole or transfers nothing and reports `EAGAIN`; a larger write may transfer part of it. A client reading lines can survive "no reply" but not "half a reply", so a line that cannot be one atomic write is refused before it is attempted. Every reply a well-framed command produces is far below the cap (the longest is `calls`, sixteen counters in well under a kilobyte). The one reply that can exceed it is the `ERR unknown-command <verb>` echo from a client that has lost its framing, since a verb may be as long as `MAX_COMMAND_LINE_LENGTH`; that reply is refused and the session ends with `EXIT_CONTROL_CHANNEL_FAILED`, the loud outcome such a client has earned.
- `BINDER_DRIVER_PATH`: the linked libbinder aborts the whole process when it cannot open its driver, so on a host without kernel binder support an unguarded service-manager call would kill the program outright, with no diagnostic, no exit code, and nothing for the parent to report but a timeout. This single check is not the middleware's bounded preflight, which also requires the driver's protocol version to equal libbinder's and waits for binder handle 0 under a bound; neither is performed here (see the file-block warnings for what that leaves exposed). The node name is spelled here because it cannot be shared: the fake's copy is file-local to its implementation and the middleware's is production code this binary deliberately does not link.

### Exit codes

- `EXIT_FAILURE` (plain 1) is deliberately never returned. Every failure class has a number of its own, so a parent, a CI log or a person reading an exit status can tell "a stale service was already published" from "the readiness pipe was wrong" without reading the output.
- `EXIT_BAD_READY_FD`: the spelling of the value is what this code answers for. A value that parses and then turns out not to be writable is a different failure, found at the readiness write and reported as `EXIT_READINESS_WRITE_FAILED`.
- `EXIT_READINESS_WRITE_FAILED`: this is also where a readiness descriptor that parsed but is not open for writing arrives, since `resolveReadinessFd()` checks only the spelling. The fake is published by then, and the process exits rather than serve an invocation whose parent can never learn it is ready.
- `EXIT_SETUP_FAILED`: covers the failures that are neither configuration nor binder state (the self-pipe, the signal handlers, constructing the fake, a service manager that cannot be reached at all). Each means the host never became able to serve, so none may exit zero.
- `EXIT_BAD_CONTROL_CHANNEL`: covers every way the channel can be asked for and not be usable: one variable without the other, a value that is not a plain non-negative descriptor number, a descriptor number not open in this process, and a descriptor open in the wrong direction (a control descriptor that cannot be read or an observation descriptor that cannot be written). Each means the parent intends to drive the host and cannot, so none may exit zero and none may write the readiness token.
- `EXIT_CONTROL_CHANNEL_FAILED`: distinct from `EXIT_BAD_CONTROL_CHANNEL`, which is a configuration fault found before the host became ready. This code means the channel was accepted and then stopped working: `poll()` or `read()` failed for a reason other than an interruption, a reply was not delivered within the `OBSERVE_WRITE_TIMEOUT_MS` deadline, or a reply exceeded `MAX_REPLY_LINE_LENGTH` and was refused rather than fragmented. A parent that has closed its ends deliberately is not this case: that is end of file or a broken pipe, and both are clean shutdowns.

### Signal-handler state (`g_shutdownSignalNumber`, `g_shutdownPipeWriteFd`, `g_shutdownPipeReadFd`)

- This is the only state in the file that is not local to a function. The objects the handler touches are `volatile sig_atomic_t` because a handler may run between any two instructions of `main`, and nothing weaker is guaranteed to be read or written indivisibly.

### handleShutdownSignal

- The whole of the handler: record the signal number, then write one byte to the self-pipe so whichever waiter is running completes: the blocking read in `waitForShutdownSignal()` when no channel was supplied, or the `poll()` in `serveControlChannel()` when one was, which is why that loop watches the pipe alongside the channel.
- Nothing else may happen there. The handler runs asynchronously between arbitrary instructions, so a stream insertion, an allocation or a binder call could deadlock against a lock the interrupted code already holds. Both objects it touches are `volatile sig_atomic_t` and `write()` is async-signal-safe.
- `errno` is saved and restored around the write, because the interrupted code may be examining its own `errno`.
- On return the running waiter completes and `g_shutdownSignalNumber` names the signal. The exit trace is emitted by `main` after the wait returns rather than from the handler, since adding any call, tracing included, would break async-signal safety.

### installShutdownHandlers

- The shutdown path is established before anything else, so a signal arriving during registration or while the readiness line is written still produces the clean exit the parent expects instead of the default disposition's abrupt kill.
- `SIGTERM` is what the parent sends at teardown. `SIGINT` is handled beside it so a person running the binary by hand stops it through the same clean path the suite uses, rather than an exit route the suite never takes.
- Neither handler sets `SA_RESTART`: an interrupted wait is retried explicitly, which keeps the interruption visible in one place instead of relying on the kernel to hide it.
- If it reports false the program has no clean way to stop and must not continue. On success both self-pipe descriptors are open and a termination signal ends whichever wait is running.

### parseDescriptorNumber

- It is the one strict parser every descriptor-valued variable goes through, so the readiness descriptor and the two channel descriptors cannot drift into accepting different spellings of the same mistake.
- `strtol()` alone would not do: it skips leading whitespace and accepts a sign, so `" 7"` and `"+7"` would parse as 7 and `"-1"` as a negative descriptor the caller would then have to catch. Requiring the first character to be a digit rejects all three. A correctly formatted descriptor number never needs that latitude; a value that does is a botched handoff.

### isUsableDescriptor

- A parent that names a descriptor it never passed, or passes one created with `O_CLOEXEC` and forgets to clear the flag on the child's copy, leaves the host holding a number that is not open at all. Discovering that at the first read would waste the parent's whole timeout, so it is established up front with a call that has no side effect.
- Direction is checked as well as openness. A pipe end has one direction, and a control descriptor that is a write end, or an observation descriptor that is a read end, means the parent crossed them over, which is worth naming rather than discovering as an `EBADF` three commands later.
- `F_GETFL` is used instead of a trial read or write because it reports the access mode without consuming a byte, without blocking, and without any side effect.

### resolveReadinessFd

- The variable is parsed strictly and in full: only an unadorned run of decimal digits is accepted, so an empty value, leading whitespace, a sign, a trailing character, a negative number and a value too large to be a descriptor are all rejected rather than coerced.
- The asymmetry between "unset" and "set to nonsense" is deliberate. Unset means nobody is listening on a pipe, which is the case when a person runs the binary from a shell, so the token goes to standard output where they can see it. Nonsense means a parent intended to be signalled on a pipe and the handoff was botched; answering on standard output would leave that parent waiting out its entire timeout unexplained, so it fails immediately and says what the value was.
- On success the descriptor is either the number the parent named, unexamined beyond its spelling, or `STDOUT_FILENO`.
- Only the spelling is established: `parseDescriptorNumber()` does not report whether the descriptor is open or writable. The channel descriptors are held to the stronger standard (`isUsableDescriptor()` as well) because the host reads and writes them for the whole session, whereas the readiness descriptor is written once, and that write establishes its openness: it traces the `errno` and exits `EXIT_READINESS_WRITE_FAILED`, so the parent still learns from an exit code rather than its own expired timeout.
- The value is read before the fake is published and long before the readiness line is written, so a misspelled value costs an exit code and no publication. Diagnostics on standard output and the shutdown self-pipe precede it; nothing a parent waits on does.

### resolveControlChannelFds

- The two variables travel together and there is no third outcome: a channel with only one half is not a degraded channel, it is one whose replies would go nowhere or whose commands would never arrive.
- Every rejected case is rejected loudly, before the host takes any action a parent could observe and long before the readiness token, so a botched handoff costs only an exit code. Serving without the channel a parent asked for would be the worst outcome: the run would appear to start, the client's first bounded wait would expire, and nothing would say why.
- Rejections: one variable without the other, a value that is not a plain descriptor number, both naming the same descriptor (a single descriptor would have the host reading its own replies), a descriptor not open in this process, or one open in the wrong direction.
- On success with no channel, the host behaves exactly as it did before the channel existed, which keeps an invocation that supplies neither variable unaffected.
- A descriptor created with `O_CLOEXEC` does not survive the exec unless the parent cleared `FD_CLOEXEC` on the child's copy first. That omission arrives as "not open in this process" and is reported as such, naming the descriptor number, because it is the single most likely handoff mistake.

### ignoreBrokenPipeSignal

- Writing to a pipe whose reader has closed raises `SIGPIPE`, whose default disposition terminates the process outright: no diagnostic, no exit code, nothing for the parent to report. A parent that closes its channel ends the session legitimately, so the host must see an `EPIPE` it can classify and act on.
- The disposition changes only when a channel was supplied; a run without one keeps the defaults it has always had, which is what "both variables unset behaves exactly as before" means in practice.
- If it reports false, a closed pipe could still take the process down, so the host must not proceed.

### writeAllRetryingOnInterrupt

- A single `write()` may transfer fewer bytes than it was given, or fail outright when a signal arrives mid-call, and the readiness token is the one thing that must arrive whole: a parent matching a line cannot match half of one. So it loops until every byte is gone, retrying an interrupted call and advancing over a partial one.
- The write is raw and unbuffered on purpose: a token handed to a C++ stream could sit in its buffer while the parent waits, which presents as a hang and not as a bug.
- A zero-length transfer on a positive request makes no progress, so retrying it would spin forever; it is reported as `EIO` instead.

### isBinderTransportPresent

- The linked libbinder treats a driver it cannot open as fatal and aborts, so on a host with no kernel binder support an unguarded service-manager call would take the program down with no diagnostic and no exit code of its own. Checking the node first turns that into a traced failure the parent can report, and keeps the host's failure disposition the same as the fake's own registration routine, which refuses for the same reason rather than aborting.
- It is a node check and nothing more: it does not establish that the driver's protocol version matches the one libbinder was built for, nor that a service manager is running. A node with no service manager behind it still blocks, and the parent's bounded readiness timeout is the guard for that.

### verifyServiceNameIsFree

- A stale registration left by another process (an earlier host that was never reaped, or a real HAL on a device) would make the middleware's lookup resolve against that service instead of this host's fake. The invocation would still run, and its result would describe something nobody chose. So it is a hard failure, not a condition to work around: publishing over the entry would hide the collision, and tolerating it would make a green result meaningless.
- `checkService()` answers immediately with null when the name is free. `getService()` would poll for seconds before concluding the same and can race with a service registering concurrently, which is precisely the ambiguity this check removes.
- The service name is obtained from the generated interface rather than spelled here. `EXIT_SETUP_FAILED` means no service manager could be reached, so whether the name is free is unknown.
- It must run after `isBinderTransportPresent()` has reported true; calling it first risks the abort that check prevents.

### waitForShutdownSignal

- It is a genuine blocking wait on the self-pipe: the process consumes no CPU while it serves transactions on its binder threads and holds this thread still. There is no timed loop and no polling interval, because a wait that wakes on a timer can be wrong about when to stop.
- A read interrupted by the very signal being waited for is retried, and the byte the handler wrote is then there to be read. A read that reports end of file also ends the wait: it can only mean the write end was closed, and there is nothing left to wait for.
- Reporting false means the self-pipe could not be read, an internal failure rather than a shutdown.
- It is the wait used when the parent supplied no channel, and it is unchanged from the program's original behaviour for exactly that reason. With a channel, `serveControlChannel()` waits instead and watches the same self-pipe, so a termination signal is answered either way.

### ReplyOutcome

- Three outcomes, because they demand three different responses from the caller; collapsing any two would either hide a failure or report a deliberate shutdown as one.
- `PARENT_GONE`: the parent closed its end of the observation pipe, a legitimate end of session indistinguishable in intent from end of file on the control descriptor, so the caller shuts down cleanly and exits `EXIT_SUCCESS`.
- `FAILED`: the write failed for a reason other than an interruption or a broken pipe, the whole-call deadline expired with the line unfinished, or the line exceeded `MAX_REPLY_LINE_LENGTH` and was refused rather than fragmented. The client is waiting for a line it will never receive, so the caller exits `EXIT_CONTROL_CHANNEL_FAILED` rather than continue a session that has silently desynchronised.

### writeReplyLine

- It appends the single newline that terminates a reply; callers pass the text without it, so one place decides how a reply is framed. It delivers the whole line or reports why not, never blocking indefinitely, and writes raw and unbuffered so a reply cannot sit in a stream buffer.
- Three properties of the function, not of the descriptor, make the bound real:
  1. One deadline for the call. A `CLOCK_MONOTONIC` instant `OBSERVE_WRITE_TIMEOUT_MS` ahead is computed on entry, and every wait gets the milliseconds remaining, clamped at zero. An interruption or a partially accepted line therefore costs only the time it consumed. A per-attempt timeout would restart with each short write and each interruption, so a client accepting a byte at a time, or a regularly arriving signal, could hold the program for an unlimited multiple of the timeout while every individual wait stayed inside it. `CLOCK_MONOTONIC` is used because an administrator or NTP cannot step it during the wait.
  2. A nonblocking write, then the flags back as they were. `poll()` reporting `POLLOUT` promises only that a write of at most `PIPE_BUF` bytes will not block. The descriptor belongs to the caller and arrives in whatever mode the parent created it (blocking, for an inherited pipe end), so `O_NONBLOCK` is set for the duration of the call and the original flags are restored on every way out, including every failure; the caller gets back the descriptor it lent. `EAGAIN` means the line is not accepted yet and is waited on again within the same deadline: `poll()` reports a pipe writable on free space, which is not the same as room for this whole line, so `poll()` paces the retry and the deadline ends it if the room never arrives.
  3. A size cap. A line longer than `MAX_REPLY_LINE_LENGTH` is refused before any write is attempted, because a line larger than `PIPE_BUF` is no longer one atomic pipe write and a client reading lines cannot recover from half of one.
- `PARENT_GONE` is reported when the write returns `EPIPE` or the wait reports `POLLERR`, `POLLHUP` or `POLLNVAL`: none can be answered and none is a fault of this program, so the session ends cleanly on all four.
- `FAILED` is reported when the line exceeded the cap and was not attempted, the monotonic clock or the descriptor's flags could not be read or set, the deadline expired with the line unfinished, the descriptor rejected the write for a reason other than `EAGAIN`, `EINTR` or `EPIPE`, or the original flags could not be restored. A zero-length transfer is also reported as a failure rather than retried, as the readiness write does.
- Preconditions: `SIGPIPE` is ignored (otherwise a closed reader terminates the process before `EPIPE` can be observed), and the descriptor is open for writing, which `resolveControlChannelFds()` established before accepting the channel.
- The descriptor's flags are those it arrived with on every return path. The restore is a single point reached by every exit from the loop. A failure to restore is not swallowed: a delivered reply is then reported as `FAILED`, since a session continuing on a descriptor whose mode the program cannot describe would rest on an assumption it can no longer make good, while `PARENT_GONE` stays `PARENT_GONE` because that session is ending cleanly either way.
- The bound is per call, not per session. A caller answering many commands spends at most `OBSERVE_WRITE_TIMEOUT_MS` per reply, and a client that stops reading costs exactly one deadline, since the first expiry ends the session.

### bytesToLowercaseHex

- It is the protocol's one payload encoding in both directions: two digits per byte, no separators, no prefix and no uppercase, so a client can compare a reply against an expected string without normalising it.

### parseHexPayload

- It is strict about length and alphabet and nothing else: an odd number of digits cannot describe whole bytes and a non-hex character cannot be guessed at, so both are rejected rather than partially decoded. Uppercase digits are accepted even though the protocol specifies lowercase, because tolerating them costs nothing and rejecting a payload a client meant correctly costs a confusing test failure.
- The decoded bytes are never inspected. A frame's destination nibble, opcode and length are the middleware's and the assertion's business; the host delivers what it was handed.

### tokenizeCommandLine

- Repeated or mixed whitespace between tokens is harmless. An empty result means the line carried no verb, which the caller treats as "not a command" rather than as an error.

### handleControlCommand

- The whole of the protocol's semantics lives in this one function, so the vocabulary a client codes against and the vocabulary the host implements cannot drift apart across functions.
- It serves exactly the commands listed under the file block's control and observation protocol (`ping`, `deliver <hex>`, `sent-count`, `last-sent`, `open-count`, `close-count`, `listener`, `registered`, `calls` and `shutdown`) and answers any other verb `ERR unknown-command <verb>`. `calls` renders each interface's fields through `renderTransactionCounts()`, one method table per interface, so the two interfaces share one formatter.
- Two structural properties matter most. The one trigger command, `deliver`, reaches the fake through its own `fireOnMessageReceived()`, so the listener invoked is exactly the one the middleware handed to `open()` and no second delivery route is invented. The observation commands read the fake's own counters and captures through its accessors, so a client learns the fake's real state, not a copy kept in step by hand; a copy could report a transmit that never arrived, which is precisely the failure the channel exists to make impossible.
- Nothing here clears, rewinds or reconfigures the fake: every command either stimulates the receive path or reports what the fake already holds.
- No verb resets the fake, deliberately. `FakeHdmiCecService::reset()` clears the captured listener, so exposing it would let a client destroy the live session's receive path mid-run and turn every later `deliver` into `ERR no-listener` for a reason no assertion could explain. Per-case isolation belongs to the in-process fixtures, which call `reset()` directly and re-open afterwards.

### serveControlChannel

- It waits on the shutdown self-pipe and the control descriptor together with `poll()`, so neither starves the other: a termination signal is answered while a command is outstanding, and a command is answered while nothing has signalled. A blocking read on either one would make the host deaf to its parent or unstoppable.
- The binder threadpool is untouched. Transactions addressed to the fake are served by pool threads started before publication; this loop holds only the main thread and no lock of the fake's while it waits, so a command and an inbound transaction can be in flight at the same time.
- Framing is handled here only: bytes accumulate until a newline, a partial line is carried across reads rather than mis-parsed as whole, a line longer than `MAX_COMMAND_LINE_LENGTH` is answered and discarded rather than allowed to grow, and every read is retried on interruption.
- The shutdown pipe is examined first: a termination signal outranks a command queued behind it, since the parent has asked the host to stop and answering one more command would delay a teardown it is already waiting on.
- A trailing carriage return is stripped so a client framing commands with CRLF is understood rather than answered `ERR unknown-command ping\r`.
- An overlong line gets the same answer whether it is the terminator of a line already discarded for length or a whole overlong line that arrived with its terminator in one read; otherwise the cap would depend on how the client's bytes were split across reads. The bytes are not parsed: a line that long has lost its framing, and what looks like a verb at its front cannot be trusted to be one.
- When no terminator arrives within the cap, the bytes are dropped at once so the buffer cannot grow without bound, and a flag records that one `ERR command-too-long` is owed when the terminator arrives, keeping the one-reply-per-line guarantee even for a client that has lost its framing.
- Both halves of the per-command trace are untrusted and go through the renderer: the command arrived from another process and may carry any byte and be thousands of bytes long (a `deliver` with a long payload, or a client that lost its framing), and the reply is derived from it, since an unrecognised verb is echoed back in `ERR unknown-command <verb>`.
- It returns `EXIT_SUCCESS` when the session ends cleanly (a `shutdown` command, end of file on the control descriptor or `poll()` reporting its writer gone, a parent that closed the observation descriptor, or a termination signal), and `EXIT_CONTROL_CHANNEL_FAILED` when `poll()` or `read()` failed for a reason other than interruption or `writeReplyLine()` reported `FAILED` (deadline expired, line over `MAX_REPLY_LINE_LENGTH`, or the descriptor rejected it).
- It requires that `installShutdownHandlers()` reported true, `resolveControlChannelFds()` reported a channel, and `ignoreBrokenPipeSignal()` reported true.
- On `EXIT_SUCCESS`, every command read and dispatched has been answered with exactly one reply line, in order, except the reply in flight when the parent closed the observation descriptor, which the client that closed it is no longer waiting for. Commands queued behind a `shutdown`, and buffered bytes whose terminator never arrived, are deliberately not served.
- On `EXIT_CONTROL_CHANNEL_FAILED`, every command before the failing one has been answered in order and the failing one has not. The exit code is the only notice the client gets, which is why the parent's wait for a reply must be bounded and why the host never continues a session it can no longer answer on.
- End of file on the control descriptor is a clean shutdown, not a failure: it can only mean the parent closed its write end, which is a legitimate way to end the session and also what happens if the parent dies, so a host left behind by a dead parent stops instead of lingering.

### main

- It runs the startup sequence of the file block, in order, and treats every step as load-bearing: each startup failure (steps 1–7, the failed readiness write included) traces what happened, returns a code of its own and writes no readiness line, so a parent never mistakes a host that failed to publish for one that is serving. A control-channel or shutdown-wait failure in step 8 comes after readiness and exits with its own code, `EXIT_CONTROL_CHANNEL_FAILED` or `EXIT_SETUP_FAILED` respectively. Nothing is swallowed and nothing degrades quietly.
- The program takes no arguments; `argc` and `argv` are unused. Configuration comes from the environment, never the command line, so the parent's launch is a plain exec of the path in `CEC_FAKE_AIDL_HOST_PATH` with no argument contract to keep in step.
- `EXIT_SUCCESS` covers every shutdown route: `SIGTERM` or `SIGINT` in either the plain self-pipe wait or the channel loop; a `shutdown` command, acknowledged before the stop; end of file on the control descriptor, or `poll()` reporting its writer gone; or the parent closing the observation descriptor, which ends the session as deliberately as end of file does.
- `EXIT_SETUP_FAILED` covers the shutdown path, the `SIGPIPE` disposition, the fake, an unreachable service manager, and a failure reading the self-pipe in the plain wait. `EXIT_BAD_CONTROL_CHANNEL` covers one variable without the other, a value that is not a descriptor number, or a descriptor not open here or open in the wrong direction.
- Both channel variables are validated (spelling, and each descriptor's openness and direction) before the fake is published, and a rejected channel writes no readiness token, so a parent that botched that handoff learns it from an exit code rather than its own expired timeout. `CEC_FAKE_HOST_READY_FD` is held to its spelling alone: a value that parses but is not writable is found at the readiness write, after publication, and exits `EXIT_READINESS_WRITE_FAILED`.
- On every return path no readiness line has been published unless the host genuinely became ready, and no resource outlives the process: the binder registration is released with the process and the fake clears its own published pointer as it is destroyed.
- The pid is traced first because a parent's diagnostics and a person hunting an orphan both key on it. It is streamed rather than cast: `pid_t` is an integer type of unspecified width, so a cast would be redundant here or lossy elsewhere.
- The channel is resolved beside the readiness descriptor and before anything is published for the same reason: a handoff that will be rejected costs nothing when rejected before the fake exists and before a readiness token could mislead a parent.
- The service name comes from the generated interface and is never spelled here, so the host cannot drift from the name the middleware looks up. Only the service is published: a client receives its controller from the out-parameter of `open()`, so the controller is never a separately registered name.
- The fake's pointer is published with `setInstance()` so anything in the process that needs the hosted fake reaches the registered one, following the single-pointer idiom of the legacy driver double in the same directory; the strong reference held by `main` keeps it alive.
- The service-side threadpool is started before publication so no transaction can arrive with no thread to serve it. `startThreadPool()` is idempotent and the library's default governs the thread count: `setThreadPoolMaxThreadCount()` is deliberately not called, because lowering a maximum another component already established can abort the process.
- The readiness token is written raw and unbuffered; handed to a C++ stream it could sit in the buffer while the parent waits, which presents as a hang rather than a bug. The trace after the write is diagnostics and no part of the signal.
- Which wait applies depends on whether the parent supplied a channel. With one, the channel and the shutdown path are watched together so neither starves the other. Without one, it is the same blocking self-pipe wait the program has always performed, unchanged, which keeps an invocation that supplies neither variable exactly as it was.
- Teardown releases what the program took, on every path including a failed one: the published pointer is cleared so nothing can reach a fake that is going away, the self-pipe descriptors are closed, and the inherited channel descriptors are closed so the parent sees end of file promptly rather than at exit. The binder registration needs no withdrawal: the pinned service manager exposes no removal API and process exit releases it.

## tests/L1Tests/test_main.cpp

### File overview (`@file`)

- The back-end selection resolves once per process, and it resolves in this harness: the
  `LibCCEC::init()` call in `CecTestEnvironment::SetUp()` is the first thing in the binary that
  forces `Driver::getInstance()`. Its helper `resolveBackEnd` (in `ccec/src/Driver.cpp`) constructs
  both back-ends, asks the AIDL one whether its service came up, and emits exactly one
  selected-path line, which every functional suite, the coverage runner and device-level
  validation match on because the `Driver` interface has no introspection API.
- Consequences: anything that is to influence the selection must happen before that init call,
  and nothing after it can change the outcome. Registering a fake service after init leaves the
  legacy back-end selected and produces a green run that proves nothing about the AIDL path, so the
  mode handling sits ahead of init by construction, not merely "early in SetUp".
- `CEC_TEST_AIDL_MODE` is read only in this file and in `tests/L2Tests/test_main.cpp`; no case
  file and no production source reads it, and none may.
  `tests/L2Tests/ccec/test_DualPathIntegration.cpp` obtains the requested mode through the L2
  harness seam `cecL2RequestedAidlMode()`.
- Modes and the coverage-runner invocations they serve:
  - `absent` (invocation A): register nothing. An unset or empty variable means exactly this, so a
    plain `./run_L1Tests` selects the legacy back-end. The harness makes no binder call in this
    mode, but `init()` still runs the production selection: without a binder driver node its
    preflight declines before libbinder is reached, while with a node and a service manager it
    makes a libbinder `checkService()` lookup, which finds no registered HDMI CEC service.
  - `compatible` (invocation B): register an in-process fake reporting its real, frozen metadata;
    the AIDL back-end is selected.
  - `incompatible` (invocation C): register an in-process fake whose interface hash is `"-1"`. The
    service is present but rejected as incompatible, the rejection is logged and the legacy
    back-end is selected. Presence alone is not sufficient, and this mode proves it.
  - `remote`: not implemented here. It means "launch the out-of-process fake host", which is
    `run_L2Tests`' job; here it is a hard failure naming that runner.
- An unrecognised value is a hard failure, never a quiet fall back to `absent`: a typo that
  silently downgraded the run to the legacy path would report a green result for an AIDL
  invocation that never happened.
- Why an in-process fake, and its limits: libbinder resolves a name registered in the calling
  process to the local `BBinder`, so `interface_cast` returns that very object. No `Bp*` proxy is
  created, no transaction crosses the binder driver and the client threadpool is not involved,
  which is why a separate L2 tier hosts the fake in its own process.
- The in-process fake is also the only way to reach the compatibility-rejection branches:
  `halcompat::isCompatible` (`rdk-halif-aidl/common/current/halcompat.h`) rejects an empty or
  `"-1"` interface hash, and only an object whose `getInterfaceHash()` dispatches virtually can
  report such a value. A remote fake cannot, because its generated `onTransact` answers the
  metadata transactions from compiled-in constants; mode `incompatible` therefore has no L2
  counterpart.
- This file does not start the binder client threadpool. `DriverAidlImpl::open()` owns that and
  runs inside the init call; starting one here would duplicate an ownership the production
  back-end already holds.

### `#include "fake_hdmi_cec_aidl_service.h"`

- Included unqualified because `AM_CPPFLAGS` already carries `-I$(top_srcdir)/mocks/hdmicec`, the
  same route `hdmi_cec_driver_mock.h` travels. It supplies the fake, its metadata overrides and the
  registration entry point that publishes it under the production service name.

### `#include "../../ccec/src/DriverAidlImpl.hpp"`

- Reached by relative path rather than an added `-I`, the same arrangement the `ccec/` suites use
  for `DriverImpl.hpp`, one directory level shallower.
- Needed for exactly one symbol, the private `DriverAidlImpl::isBinderPreflightOk()`, called
  through `BinderPreflightTestAccess`. Reaching the service manager unguarded is unsafe in two
  independent ways on the pinned binder stack: with no driver node libbinder aborts the process
  rather than returning an error, and with a driver node but no running servicemanager it blocks
  indefinitely waiting for binder handle 0. A plain existence check on the driver node would cover
  only the first.
- `isBinderPreflightOk()` covers both (node openable, protocol version equal, handle 0 resolved
  within a bounded timeout). It is private, so this file reaches it through the befriended
  `BinderPreflightTestAccess` it defines. Using it also keeps the driver-node path out of this
  file, so there is no second spelling to drift.

### `#include "../../ccec/src/DriverImpl.hpp"`

- Included for the class only, so the per-test registry restoration can establish by
  `dynamic_cast` which back-end the process resolved to. Restoring the registry calls
  `Driver::removeLogicalAddress()`, which on the AIDL back-end issues a binder transaction this
  harness must not perform. Nothing else in the file needs the type, and no case is served by it.

### BinderPreflightTestAccess

- The test-only gateway `DriverAidlImpl` befriends so the private predicate stays reachable from
  the L1 suite. Production never defines it, so production carries no wrapper or forwarder.
- Its one static member template forwards its arguments unchanged, so a call naming no arguments
  gets the predicate's own defaults, exactly as the production call site does.
- `tests/L1Tests/ccec/test_DriverAidl.cpp` defines it with identical tokens: both units call the
  predicate, and the one-definition rule requires every definition of the class to match. Both
  definitions sit in the namespace `CCEC_BEGIN_NAMESPACE` opens, the one the friend declaration
  names.

### g_fakeAidlService

- Follows the same single-pointer idiom as `g_driverMock`.
- Deliberately never released in `TearDown`. The pinned C++ `IServiceManager` exposes no
  service-removal API; the service manager keeps a reference to the published binder and the
  middleware may hold one too. Dropping this reference would not unpublish anything; at best it
  would destroy an object the service manager still advertises. Nothing deregisters it.

### Log-injection rendering contract (RENDER_LIMIT, renderUntrustedValue)

- Defect removed (CWE-117): diagnostics used to stream caller-supplied text verbatim. A value
  carrying a newline ended the message and began a line of its own, and a line beginning
  `::error::` is a GitHub Actions workflow command. Measured before the fix:
  `CEC_TEST_AIDL_MODE=$'bogus\n::error::FORGED_L1_ANNOTATION'` produced a standalone forged
  `::error::` annotation in the run log, and a five-thousand-character value produced more than ten
  kilobytes of diagnostics, burying the real failure.
- The contract, in application order (the order makes the escaping unambiguous and reversible):
  - (a) a literal backslash becomes `\\` first, so every escape introduced afterwards is
    distinguishable from the same characters occurring literally in the value;
  - (b) `0x0A` becomes `\n`, `0x0D` becomes `\r`, `0x09` becomes `\t`, and every other byte outside
    printable ASCII `0x20..0x7E` becomes `\xNN` in lower-case hex. Classification is by byte value
    on `unsigned char` with no locale-sensitive function (no `isprint`, `iswprint` or ctype table),
    so it behaves as under `LC_ALL=C` in any locale and no multi-byte sequence can hide a control
    character;
  - (c) the rendering is truncated at `RENDER_LIMIT` characters and
    `...[truncated, N bytes total]` is appended, N being the value's own length in bytes, so a
    caller cannot drown a log and the message still says how much was withheld;
  - (d) it is applied to the value, never to the surrounding message, and the result is always
    delimited with double quotes, so an empty value is visible as `""` rather than as a gap;
  - (e) it never begins a diagnostic line: every message keeps its own prefix in front of it and the
    rendering begins with `"`. With (b) leaving no raw newline, a forged standalone `::error::`,
    `::warning::` or `::notice::` line is unreachable by construction rather than unlikely.
- This is one of five copies of one contract; they must not diverge, and a change to any is a
  change to all five:
  - `hdmicec/tests/L1Tests/run_coverage.sh` — `render_untrusted()`
  - `hdmicec/.github/workflows/aidl-path-tests-rootfs.sh` — `render_untrusted()`
  - `hdmicec/tests/L1Tests/test_main.cpp` — `renderUntrustedValue()`
  - `hdmicec/tests/L2Tests/test_main.cpp` — `renderUntrustedValue()`
  - `hdmicec/mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp` — `renderUntrustedValue()`
- The two convenience overloads are `inline` so a copy whose file does not call one of them raises
  no `-Wunused-function` warning; the copies are identical by construction, and which overloads a
  file calls is a property of its callers.
- Five file-local copies rather than one shared helper, deliberately: two are shell and three are
  C++, spread over three build targets and one non-built script, and a shared header would add a
  build-system edge for a twenty-line function. The cost is the cross-reference every copy carries.
- `renderUntrustedValue(const char *, std::size_t)`: every diagnostic in the file naming a value
  supplied by the environment, the command line or another process goes through it. Any input is
  acceptable, including embedded newlines, carriage returns, ANSI escape sequences, invalid UTF-8
  and multi-kilobyte payloads. The result is never longer than `RENDER_LIMIT` characters plus the
  truncation note and the two quotes. Rendering a whole message would escape the message's own
  punctuation and destroy the prefix clause (e) depends on.
- Backslash arm first (function body): this ordering is clause (a); it makes a rendered `\n`
  unambiguously either the two characters the value contained or a newline it contained.
- `renderUntrustedValue(const char *)`: a null pointer renders as the four characters `<unset>`,
  undelimited, because "unset" and "set to the empty string" are different facts.

### AIDL_MODE_VARIABLE and the four mode spellings

- Spelled exactly once each; the spellings are a fixed contract shared with
  `tests/L2Tests/test_main.cpp`, `run_coverage.sh`, both CI workflows and the test documentation.

### BROKEN_INTERFACE_HASH

- `halcompat::isCompatible` rejects `"-1"`; the accompanying comment in `halcompat.h` reads "hash
  RPC failed - not a dev build, a broken link". Reporting it is the only way to drive a present
  service down the incompatible arm of the selection.

### failIfServiceAlreadyPublished

- A stale registration left by another process makes the run's selection resolve against that
  service instead of the harness's fake, so the outcome would depend on what happened to be running
  on the machine. Overwriting the entry hides the collision; tolerating it makes a green result
  meaningless.
- The service name is supplied from the generated interface rather than a literal, so the harness
  cannot drift from the name the middleware looks up.
- A collision, or a service manager that cannot be reached, is raised as a fatal gtest failure.
- Called before `isBinderPreflightOk()` has passed, it would abort or block the process instead of
  failing the test.
- The pinned C++ `IServiceManager` offers no way to clear a collision.

### publishFakeForMode

- Mode `incompatible` differs from `compatible` by exactly one call, the interface-hash override,
  which keeps the two modes' divergence auditable at a glance.
- Without a usable binder transport the run fails rather than publishing nothing: publishing
  nothing would select the legacy back-end and report a green result for an AIDL invocation that
  never ran.
- Fatal failures: no usable binder transport, the name already published, a fake that cannot be
  constructed, a fake that cannot be published.
- Preconditions: it runs ahead of `LibCCEC::init()` in `CecTestEnvironment::SetUp()`, because a fake
  published after init leaves the already-resolved selection on the legacy back-end; and nothing
  may already be published under the production name, which `failIfServiceAlreadyPublished()`
  establishes.
- Nothing withdraws the registration, because the pinned C++ `IServiceManager` has no
  service-removal API.
- `setInstance` ordering (function body): `SetUp` runs before any `TEST_F` body, which lets a case
  configure or observe a fake registered long before it ran.

### resolvedAidlMode

- The single spelling of the "unset or empty means `absent`" rule. `applyAidlModeBeforeInit()`
  acts on its result before `init()`, and `failUnlessSelectedBackEndMatchesMode()` checks the
  selection against the same result after it, so the two cannot disagree about which mode the run
  was given.
- It does not validate: an unrecognised value is returned as given, and
  `applyAidlModeBeforeInit()` is where it fails the run.

### applyAidlModeBeforeInit

- The one place in `run_L1Tests` that acts on the four modes.
- Every path that does not need libbinder avoids it: `absent` returns without touching it, and both
  rejection paths (`remote`, unrecognised) fail before reaching it. Whether the run reaches
  libbinder afterwards is the production selection's decision, made in `LibCCEC::init()`: without
  a binder driver node its preflight declines first, so the default invocation runs on a host
  without kernel binder support.
- An unrecognised value, mode `remote` and any failure to publish the fake are each fatal gtest
  failures; for `compatible` and `incompatible` the process holds a reference to the fake.
- `LibCCEC::init()` is the first thing in the binary that forces `Driver::getInstance()`; the
  selection is then fixed, so a service reached after init leaves it on the legacy back-end and
  produces a green run that proves nothing about the AIDL path.
- A fatal assertion returns from this function without unwinding its caller, so
  `CecTestEnvironment::SetUp()` invokes it through `ASSERT_NO_FATAL_FAILURE`.
- Rendered unrecognised value (function body): it is the one diagnostic in the binary naming a
  value nothing has validated (every earlier arm matched a known spelling). Streamed raw,
  `$'bogus\n::error::FORGED'` ended the message and began a standalone GitHub workflow command;
  `renderUntrustedValue()` leaves no newline, bounds the length and keeps the sentence's own words
  in front of the value.

### failUnlessSelectedBackEndMatchesMode

- The hard failure for a stale registration in mode `absent`. `compatible` and `incompatible`
  detect one before publishing, through `failIfServiceAlreadyPublished()`; `absent` publishes
  nothing and so performs no lookup, which left a stale "HdmiCec" service free to win the
  selection: the run then executed on the AIDL back-end and, under any filter that excluded the
  legacy-bound suites, reported green. Reading the outcome after `init()` closes that gap without
  this harness reaching the service manager itself.
- Identity is a `dynamic_cast` against `DriverImpl` and `DriverAidlImpl`, the two concrete types
  `Driver::getInstance()` can return. That is a vtable lookup, never a binder call, and it needs no
  production introspection API. Every arm also catches a selection that contradicts its mode for
  any other reason, naming the mode, the back-end required and the back-end selected.
- The failure is raised in the global environment's `SetUp()`, so no case body runs and the binary
  exits non-zero; `TearDown()` still runs and already tolerates a partial setup.

### LogicalAddressRegistryGuard

- The driver's list of acquired logical addresses is the only piece of its state a test can add to
  that nothing takes away again. `DriverImpl::close()` deliberately does not clear it, the AIDL
  back-end matches that, and clearing would be an unauthorized change to legacy behaviour.
- Two groups of cases sit on opposite sides of this: some register an address and do not remove it
  in teardown; others assert that an address is not registered, because
  `Connection::matchSource()` only rewrites a frame's source nibble when
  `Driver::isValidLogicalAddress()` reports the address as acquired.
- In declaration order the negative-precondition cases run first and everything passes. Under
  `--gtest_shuffle` (a valid order CI may use) the registering cases can run first and the others
  then fail on a rewritten source nibble. Measured: seeds 12345 and 99999 each failed exactly two
  cases; seed 54321 passed. A suite whose verdict depends on its order cannot certify anything.
- Why a listener rather than a fixture teardown: the twelve pre-existing L1 units are outside the
  migration's diff by design (an acceptance check enforces that), and a teardown in the two
  registering fixtures would fix only those two and leave every other and every future fixture
  free to reintroduce the leak. A process-global listener covers them all: after every case it
  reports each address registered beyond the first case's baseline and, on the legacy back-end,
  tries to remove it.
- It detects and reports; it does not restore the baseline exactly. A baseline address a case
  removed or replaced is not re-added, and an added address it cannot remove (an AIDL selection, a
  driver that is not OPENED, a removal that raises) is logged and left registered, so each later
  case's report names it again until something removes it.
- It does not touch production close/term semantics: legacy removal goes through the driver's
  public interface from outside the driver, and no production file changes.
- It does nothing when nothing leaked, the case after all but a handful of tests. Detection is a
  pure list walk under the driver's lock (`Driver::isValidLogicalAddress()` reaches no HAL and no
  service on either back-end), so the common path costs fifteen list walks and no HAL call.
- It issues no binder call, ever: `Driver::removeLogicalAddress()` on the AIDL back-end is a
  transaction, so removal is attempted only on the legacy back-end, and under an AIDL selection a
  residual registration is reported and left. The order dependence is a legacy-invocation problem
  anyway: the leaking cases are the `LibCCEC` ones, which the AIDL invocations' filters exclude,
  and the AIDL session fixture re-registers the device's address around every case.
- Superseded: the pre-refine comment said the AIDL session fixture's cases add and remove their
  addresses within a single case. The AIDL back-end now registers the device's address when the
  driver is enabled, and the fixture's close-open cycle in `SetUp` and `TearDown` re-registers it.
- It never fails a test: everything it calls is wrapped, because a removal problem must not be
  attributed to a case that already produced its own result. It reports on stdout instead.
- The gmock warning: removal on the legacy back-end reaches `HdmiCecRemoveLogicalAddress()` on
  the process-global mock with no expectation, so gmock prints "Uninteresting mock function call"
  and takes the mock's `ON_CALL` default (`HDMI_CEC_IO_SUCCESS`, installed in
  `hdmi_cec_driver_mock.cpp`). This is accepted rather than silenced. Installing a permissive
  `EXPECT_CALL` and then calling `::testing::Mock::VerifyAndClearExpectations()` was measured and
  rejected: it clears every expectation live on the mock, including one a case legitimately left
  unmet, and could hide a real failure. `::testing::Mock::AllowUninterestingCalls()` would express
  this exactly but is private in GoogleTest 1.15. An explanatory line is logged immediately before
  the removals.

### LogicalAddressRegistryGuard::LogicalAddressRegistryGuard

- The baseline is taken lazily at the first test, deliberately: a run whose environment `SetUp`
  failed fatally never starts a test, and the guard must not be what forces the back-end selection
  on such a run.

### LogicalAddressRegistryGuard::OnTestStart

- `testInfo` is unused; the first call is what matters, not which case it belongs to.
- The baseline is captured rather than assumed empty, so an address `init()` itself acquired is
  treated as part of the starting state instead of being torn out from under every case.
- Superseded: the pre-refine comment said no address is acquired by `init()` ("none does today").
  That no longer holds on the AIDL back-end, which registers the device's DeviceType-derived
  logical address when the driver is enabled inside `init()`; the capture-not-assume design covers
  it unchanged.

### LogicalAddressRegistryGuard::OnTestEnd

- `testInfo` is named in the report so a leak is attributed to the case that made it rather than
  the case that would have tripped over it.
- GoogleTest sequences listener `OnTestEnd` after the case's own `TearDown`, so a fixture that
  cleans up after itself has already done so.
- Baseline addresses are neither removed nor re-added, so one the case removed or replaced stays
  missing. Every other registered address is reported; on the legacy back-end its removal is then
  attempted and a removal that raises, or an address still registered afterwards, is logged. Under
  an AIDL selection the addresses are reported and left registered, with no binder call.
- Each legacy removal produces one gmock warning; the class notes record why that is accepted.

### LogicalAddressRegistryGuard assignable-address enum

- `0x0` to `0xE` inclusive. `0xF` is UNREGISTERED/BROADCAST, a destination and never an address a
  device acquires; the AIDL controller documents the same range for `addLogicalAddresses()`, so
  probing it would ask about a value neither back-end can hold.

### LogicalAddressRegistryGuard::isRegistered

- Returning false when the query itself failed is the safe direction: it leads to no removal.
- `Driver::isValidLogicalAddress()` is a list walk under the driver's own lock on both back-ends,
  reaching no HAL, service or binder transaction, which makes probing every address after every test
  free. It does not check lifecycle state, so it answers on a closed driver as on an open one, which
  matters because the registry survives a close.

### LogicalAddressRegistryGuard::restore

- When it returns false nothing was called, and the caller must not report a residual registration
  as the failure of a removal that never ran.
- `InvalidStateException` is expected rather than exceptional: the driver must be OPENED for a
  removal, and a case that terminated the library leaves it CLOSED. It is reported and the loop
  continues, because the remaining addresses are worth attempting and nothing the guard does may
  fail a test.

### LogicalAddressRegistryGuard::baselineRegistered

- Captured rather than assumed empty, so an address the starting state holds is never removed: the
  device's registered address on the AIDL back-end, none on legacy.
- Asserting that the baseline is empty would be asserting a property of `LibCCEC::init()` from the
  wrong place.
- Superseded: the pre-refine comment called the baseline "empty in practice on this binary". Under
  an AIDL selection it can now hold the address `init()` registers from the device's DeviceType; on
  the legacy back-end it remains empty.

### CecTestEnvironment::SetUp

- The mode is applied before `init()` because `init()` is the one-way door that fixes the
  selection; a failure stops the run rather than letting `init()` resolve a selection the requested
  mode did not ask for.
- Init failure is fatal to the whole run: `SetUp` runs once for the binary and every
  driver-dependent case takes an initialized CEC stack as its precondition, so carrying on would
  assert against an unopened stack and report green for a process that never came up.
- Nothing can legitimately be ignored: `init()` raises `InvalidStateException` on a second call,
  which a once-per-process `SetUp` never reaches, and the exceptions it can raise
  (`Driver::getInstance().open()` refused by the HAL, `Bus::getInstance().start()` failing) are
  real failures.
- Immediately after `init()`, `failUnlessSelectedBackEndMatchesMode()` holds the resolved selection
  to the mode, so a stale registration under `absent`, or any selection the mode did not ask for,
  fails the run before a case executes.

### CecTestEnvironment::TearDown

- `TearDown` runs after the suite, so every result has been recorded; aborting would obscure
  legitimately earned results, and a fatal assertion would cut the three cleanup statements short.
  A non-fatal expectation makes a failing `term()` visible while the cleanup completes.
- One failure is expected and honest: when `init()` failed in `SetUp`, `term()` raises
  `InvalidStateException` and is reported as a second failure, which correctly says the process
  never came up.
- Nothing unpublishes the fake AIDL service; see `g_fakeAidlService`.

### main

- `LogicalAddressRegistryGuard` makes the binary's result independent of case order, including
  under `--gtest_shuffle`, which used to fail two cases on two of three sampled seeds.
- It is appended so it runs after the default result printer's `OnTestEnd`: the case's `[ OK ]` or
  `[ FAILED ]` line is printed first and any guard report appears beneath it. GoogleTest takes
  ownership of the listener, so nothing deletes it.

## tests/L1Tests/ccec/test_DriverAidl.cpp (part 1 of 6)

Detail moved out of the comments in the file header, the contract and manifest blocks, the
file-scope constants and the helpers through `ScopedCecLogLevel`.

- **Superseded, recorded only as such.** The AIDL back-end's `getPhysicalAddress()` now reports
  the fixed physical address 1.0.0.0 (`0x01000000`) with no AIDL call, so these former
  statements no longer hold: the file header's unreachable path "a physical-address read on the
  AIDL back-end, blocked item B1"; the contract block's "What is covered here and what is
  blocked" paragraph (B1, the device-settings HAL EDID-byte read whose header was never
  supplied, every alternative route forbidden, the block logged and the caller's out-parameter
  left untouched, SC6(f) a partial discharge) and its "Required change, reported not made"
  paragraph; and the manifest's "not established: the AIDL physical address, blocked on B1"
  entry. SC6(f) is now a both-paths assertion. Removing that item renumbered the former paths 6
  and 7 to the 5 and 6 listed below.

### File header (`@file test_DriverAidl.cpp`)

- Three things are exercised by three routes, because no single route reaches all of them:
  1. The compatibility predicate and the binder preflight, by direct call against locally
     constructed doubles and against paths the file owns. Neither needs a registered service, a
     binder driver or a resolved back-end, so both run under every invocation.
  2. The AIDL back-end's closed-state behaviour, on a local instance.
     `DriverAidlImpl::DriverAidlImpl()` touches no binder: it sets the state to CLOSED, the
     legacy handle field to 0 and an empty address list, exactly as `DriverImpl::DriverImpl()`
     does (`DriverImpl.cpp:87-90`). A local instance is therefore constructible where no service
     exists, so every `status != OPENED` guard, the `writeAsync()` prelude ordering and the
     unguarded methods are reachable without a HAL. Local probes (`ReceiveQueueProbe`,
     `SessionStateProbe` and its `AllocationProbe` subclass) also put a local instance in OPENED
     by forcing or injecting session state, and their destructors return it to CLOSED.
  3. The back-end actually resolved for the process, through `Driver::getInstance()`. Which one
     that is depends on the invocation, so cases needing a specific one live in their own
     fixtures and assert their precondition in `SetUp`.
- Isolation: the suite shares one process-global driver with every other suite in the binary,
  and the binary is order-sensitive. Cases needing an open driver establish that precondition
  themselves, leave the driver open again (the state the global environment sets up) and clear
  their own mock expectations. Cases needing a driver that is not open construct a local
  `DriverAidlImpl`. No case closes, terminates or re-initialises the shared library, and none
  installs a default action on the process-global HAL mock.
- The one other piece of process-global state touched is the middleware log level, which one case
  raises to DEBUG so a `LOG_DEBUG` callback report can be observed. The only seam is
  `check_cec_log_status()` and the fixed, host-shared path it reads, so `ScopedCecLogLevel` locks
  that path, validates it, replaces it atomically, restores it byte for byte on every exit path,
  and refuses rather than forces when the path is not safely the run's to modify. No later case
  inherits a raised level.
- Reachability notes, moved from the file header (which keeps paths 1 and 2 in brief and points
  here):
  1. Older-same-major. This arm of halcompat's era-0 rule is empty for this client, not merely
     untested. `IHdmiCec::VERSION` is 1000 (`IHdmiCec.h:25`), which under the positional encoding
     (`halcompat.h:86-96`) is era 0, major 1. The servers satisfying `era(server) == 0` and
     `major(server) == 1` are exactly 1000 through 1999, all `>= 1000`, so the
     `serverVersion >= clientVersion` conjunct (`halcompat.h:114`) cannot fail while the two
     preceding conjuncts hold. A server reporting 999 decodes to era 0, major 0 and is rejected by
     the cross-major conjunct (`halcompat.h:113`), so calling such a case "older same-major" would
     record a rule that does not exist. The arm is reached instead with a different client, by
     calling `detail::isCompatible(3020, 3000)` directly; a static assertion pins it and explains
     why `halcompat.h:128`'s own `(3000, 2000)` assertion is mislabelled.
  2. The era >= 1 branch of the ternary (`halcompat.h:110-111`) requires `era(clientVersion) >= 1`.
     This client's era is 0 while `IHdmiCec::VERSION` is 1000, so no server value can activate
     it. Reaching it needs an interface re-frozen in era 1 or later, at which point the static
     assertion on `VERSION` fires and the analysis is recomputed.
  3. The preflight's protocol-version-mismatch and context-manager arms are reachable on any host
     through the probe seam, so they are a constraint on how they are reached rather than
     absences. Neither can be synthesised from a path and a timeout alone: the first needs a node
     answering `BINDER_VERSION` with a value other than the compiled one, the second a driver whose
     context manager never answers, and a regular file is refused at the character-device check
     (decision point 4), before the protocol read. The predicate therefore takes a third,
     defaulted argument, a
     `DriverAidlImpl::BinderPreflightProbe` of six function pointers defaulting to the real
     syscalls. A synthetic probe reaches every arm on any host, including a true verdict, and
     `DriverAidlPreflightTest` asserts all eight decision points (the node's identity, file type
     and ownership among them) together with the custody handover and the pre-lookup
     re-verification that narrows the window between the check and libbinder's own open of the
     same name. The production call site passes no arguments and runs the real syscalls.
  4. Withdrawing a registered service. The pinned C++ `IServiceManager` exposes no
     service-removal API and the service manager retains whatever was published, so a test built
     around unregistering cannot be written; dropping the local reference would at best destroy
     an object the service manager still advertises. The selection-stability case therefore adds
     a service mid-process instead, the direction that is expressible.
  5. The body of `DriverAidlImpl::printFrameDetails()`'s `catch (Exception &e)`. The only
     statement in the guarded `try` that can throw is the `Header` construction, which reads
     `frame.at(0)` and raises `std::out_of_range`; `Exception` and `std::out_of_range` are
     siblings under `std::exception`, so the handler cannot catch it and it escapes. Reaching the
     handler needs a production change (a broader handler, or a length guard ahead of the
     `Header`). The method is byte-for-byte the legacy one, whose identical boundary
     `test_DriverImpl_Async.cpp:38-46` records, so this is a shared pre-existing condition. The
     escape is asserted by the `writeAsync` prelude-ordering cases, so any change to the handler
     is a visible decision.
  6. The non-CLOSED arm of `DriverAidlImpl::~DriverAidlImpl()`, which closes an open session, is
     reachable, but no designed case takes it on a passing run. Local instances do reach OPENED:
     `ReceiveQueueProbe::markOpened()` forces it, and `SessionStateProbe::injectOpenSession()`,
     inherited by `AllocationProbe`, injects a session. Each probe's own destructor sets CLOSED
     (the receive probe after draining its queue) before the base destructor runs, so the arm's
     `status != CLOSED` test is false for them. Under invocation B,
     `DriverAidlSessionTest.AFailedCloseStillReleasesAReaderParkedOnTheIncomingQueue` and
     `DriverAidlSessionTest.ACallbackAfterAFailedCloseOrOwnerDestructionIsDroppedNotDelivered`
     resolve and open a plain local `DriverAidlImpl` against the in-process fake, whose `open()`
     does not enforce the real HAL's single-session rule (`EX_ILLEGAL_STATE`, `IHdmiCec.aidl:93`).
     Each closes it before it goes out of scope, and a failed `close()` sets CLOSED before it
     raises, so the destructor skips its close; only a fatal assertion between that `open()` and
     `close()` would destroy it OPENED. The shared driver is not destroyed OPENED either:
     `CecTestEnvironment::TearDown` (`tests/L1Tests/test_main.cpp`) calls `LibCCEC::term()`,
     which closes it (`ccec/src/LibCCEC.cpp:122`) before static destruction. A case destroying a
     still-OPENED local instance, such as a probe without the forcing destructor or a plain
     instance opened under B and left open, would take the arm with no new production seam, since
     the probes already inject a proxy through the back-end's protected members. The arm has no
     `run_coverage.sh` `BRANCH_MANIFEST` record, so the gate neither requires nor reports it;
     whether a coverage run marks it taken is a trace measurement that no case establishes. The
     destructor takes the instance lock and then calls `close()`, which takes it again; that is
     safe because `CCEC_OSAL::Mutex` is created `PTHREAD_MUTEX_RECURSIVE_NP`
     (`osal/src/Mutex.cpp:44`), and `DriverImpl`'s destructor has the same shape. Superseded: this
     note formerly said no destroyable instance is ever OPENED and that reaching the arm needs a
     new production seam; neither holds.
- The header formerly also cited `CecTestEnvironment::SetUp` and `DriverImpl` as see-also targets.

### Include rationale

- `DriverImpl.hpp` lives under `ccec/src`, which `AM_CPPFLAGS` does not cover, so it is reached by
  relative path rather than an added `-I` (the route `test_DriverImpl_Async.cpp:75` takes). It is
  needed to name the legacy concrete type in a `dynamic_cast`, which is how cases establish the
  resolved back-end without a production introspection API on `Driver.hpp`.
- `DriverAidlImpl.hpp` is absent from `hdmicec/Makefile.am`'s installed `nobase_include_HEADERS`,
  exactly as `DriverImpl.hpp` is, which is what makes a second back-end possible without altering
  the public API and reaching it from a test translation unit legitimate. It is needed to call
  the private static `DriverAidlImpl::isBinderPreflightOk()` through the befriended
  `BinderPreflightTestAccess` this file defines, to name the AIDL concrete type in a
  `dynamic_cast`, and to construct local closed instances that reach the state guards and
  prelude ordering without touching the shared driver.
- `binder/ProcessState.h` serves the one observation that the binder threadpool exists. It is
  reached only inside a `DriverAidlSessionTest` body, never at file or fixture scope, and only
  through `selfOrNull()`: on a driverless host merely reaching `ProcessState::self()` raises
  SIGABRT in the pinned build, `DriverAidlSessionTest.*` is excluded from the driverless
  invocation by the runner's filter, and `selfOrNull()` returns null rather than creating a
  `ProcessState`, so it cannot open a driver that is not there.
- `halcompat.h` is spelled with quotes because `ccec/src/DriverAidlImpl.cpp` spells it so. It is
  in neither frozen snapshot include root but at `HALIF_PREFIX/common/current`, which is why
  configure resolves that third root separately (its "for the halcompat.h include root" check in
  `configure.ac`) and why the include fails with the two snapshot roots alone.

### `cechal` and `halcompat` namespace aliases

- `cechal` is a namespace alias rather than using-declarations for individual types for the reason
  the production back-end records against its own alias: the middleware compiles with
  `CCEC_NAMESPACE` undefined, so its names sit at global scope, and pulling generic names such as
  `State` in beside them invites a collision a later header could create silently.

### BinderPreflightTestAccess

- The befriended test-only gateway to the private `DriverAidlImpl::isBinderPreflightOk()`; the
  `DriverAidlPreflightTest` cases and
  `DriverAidlSelectionTest.RegisteringAServiceMidProcessDoesNotChangeTheResolvedBackEnd` call the
  predicate through it.
- `tests/L1Tests/test_main.cpp` defines it with identical tokens, as the one-definition rule
  requires of a class both units define. It sits at namespace scope, outside every anonymous
  namespace, so it is the class the friend declaration names; see the `test_main.cpp` entry.

### AIDL contract check

- Every figure in the table comes from one of four authorities; where two disagree, the
  disagreement is a declared difference between the back-ends rather than a defect:

  | Value | Figure | Source |
  |---|---|---|
  | `IHdmiCec::VERSION` | 1000 | `IHdmiCec.h:25` |
  | `IHdmiCec::HASHVALUE` | `70a901cf...` | `IHdmiCec.h:27` |
  | `SendMessageStatus` | 0 / 1 / 2 | `SendMessageStatus.h`: `ACK_STATE_0`, `ACK_STATE_1`, `BUSY`, in that order |
  | `REPORT_PHYSICAL_ADDRESS` | `0x84` | `ccec/include/ccec/OpCode.hpp:73` |
  | `CECFrame::MAX_LENGTH` | 128 | `ccec/include/ccec/CECFrame.hpp:55-57` |
  | AIDL `sendMessage` maximum | 16 | `IHdmiCecController.aidl`: "The maximum message size (Header Block plus opcode block plus operand blocks) is 16 * 8 bits (16bytes)", enforced by `DriverAidlImpl::write()` against the file-local `AIDL_MAX_MESSAGE_LENGTH` in `ccec/src/DriverAidlImpl.cpp` |
  | Legacy HAL frame maximum | 20 | the legacy HAL specification |

- The frame-size conflict is not resolved: each contract is authoritative for its own back-end and
  no divergence settles the overlap, so a 17- to 20-byte frame is sendable on legacy and raises
  `IOException` on AIDL. That is an authorized observable difference, and the important half is
  what does not happen: the frame is policed, never truncated, because a truncated CEC frame on
  the bus is worse than a refusal. The cases assert both halves, the exception and that nothing
  reached the fake's `sendMessage`.
- The inverted ACK sense is the crux of the adaptation. `ACK_STATE_0` means acknowledged for a
  directed message and rejected for a broadcast; `ACK_STATE_1` is the mirror. The destination
  nibble deciding which reading applies is `frame.at(0) & 0x0F`, broadcast being `0x0F`, read as
  the legacy implementation reads it. The translation lives in `DriverAidlImpl::write()`, each arm
  reproducing one arm of the legacy mapping in `DriverImpl.cpp`: `:261-263` (HAL error →
  `IOException`), `:265-274` (the five-value send-failed family → `IOException`), `:276-278`
  (directed not acknowledged → `CECNoAckException`) and `:279-284` (the CEC CTS 9-3-3 arm: a
  rejected broadcast `REPORT_PHYSICAL_ADDRESS` raises `CECNoAckException` so the caller retries,
  while every other rejected broadcast opcode returns normally).
- On enable, `DriverAidlImpl::open()` discovers the device's one logical address from
  `DriverAidlImpl::LOCAL_DEVICE_TYPE` (`DeviceType::PLAYBACK_DEVICE`). It polls candidates 4, 8
  and 11 in that order with the self-addressed `poll(c, c)`, takes the first whose poll raises
  `CECNoAckException` (nothing answered, so the address is free), and registers it with one
  one-element `IHdmiCecController::addLogicalAddresses()` call. The back-end's list never holds
  more than one address: an explicit `addLogicalAddress()` of a different one releases the held
  one first. `getLogicalAddress()` reads the registered address back through
  `IHdmiCec::getLogicalAddresses()` on every call, never from the local list. The allocation
  rules, failure arms and test double are in "Logical-address allocation and registration (AIDL
  back-end)".
- Physical-address retrieval is covered on the legacy back-end, where the legacy mock's
  `HdmiCecGetPhysicalAddress` expectation is met and the value reaches the caller.
- On the AIDL back-end, `getPhysicalAddress()` writes the fixed 1.0.0.0 (`0x01000000`) in every
  driver state (never opened, OPENED, CLOSED) and makes no AIDL call; the encoding and its callers
  are in "Physical address (AIDL back-end)".
- `IHdmiCec::close()` is a provisional, not blocked, mapping: a high-confidence candidate for the
  legacy `HdmiCecClose()`, pending owner confirmation, because the HAL mapping table has no entry
  for `HdmiCecClose()` and no divergence settles it. It is used because a back-end that cannot
  close is not deliverable, and `DriverAidlImpl::close()` carries the same marker. The close cases
  assert the sequence (state CLOSED before the raise, local address list not cleared), which is
  what the legacy back-end does and stays true whichever AIDL method the owners confirm; a green
  result does not confirm the mapping.

### Fixture manifest

- The manifest table, moved here verbatim from the source block, which keeps a two-line pointer
  and invocation A's filter:

```text
  Fixture                      Cases  Back-end  Invocation  CEC_TEST_AIDL_MODE  Binder driver
  ------------------------------------------------------------------------------------------
  DriverAidlCompatibilityTest     28  any       A, B, C     any                 no
  DriverAidlPreflightTest         29  any       A, B, C     any                 no
  DriverAidlSelectionTest          4  legacy    A           absent              no
  DriverAidlLocalInstanceTest     50  any       A, B, C     any                 no
  DriverAidlLegacyArmTest          5  legacy    A           absent              no
  DriverAidlSessionTest           32  AIDL      B           compatible          yes
  DriverAidlTransmitTest          12  AIDL      B           compatible          yes
  ------------------------------------------------------------------------------------------
                                 160  of which 116 run under invocation A

  Invocation  Registered  Selected  Excluded
  A                  643       599        44
  B                  643       459       184
  C                  643       415       228
```

- The selection resolves once per process, inside the `LibCCEC::init()` call in
  `CecTestEnvironment::SetUp` (`tests/L1Tests/test_main.cpp`), the first thing in the binary to
  force `Driver::getInstance()`. By the time a `TEST_F` body runs the choice is made, so one run
  yields one selection outcome and every outcome needs its own process; hence fixtures are
  partitioned by invocation and each invocation-specific one asserts its precondition in `SetUp`
  rather than adapting. That `SetUp` asserts the resolved back-end by `dynamic_cast`, so a
  back-end-specific fixture run where the other back-end resolved fails there and never skips.
  `DriverAidlSelectionTest` under invocation C, where legacy also resolves, is kept out by the
  filter instead (see the invocation C entry below).
- The 116 run under invocation A are the first five fixtures, 28 + 29 + 4 + 50 + 5. Invocation A
  excludes `DriverAidlSessionTest` (32) and `DriverAidlTransmitTest` (12), the 44 excluded,
  because both require the AIDL back-end to be the resolved one.
- The runner's per-invocation gate reconciles selected plus excluded against registered, so all
  three numbers matter and a stale one fails the invocation. Registered is 643 = 483 pre-existing
  + 160, in 25 suites; selected and excluded are 599 and 44 under A, 459 and 184 under B, and
  415 and 228 under C. Every figure was measured on the host with the runner's own filters, by
  `./run_L1Tests --gtest_list_tests --gtest_filter=<filter> | grep -cE '^  [A-Za-z]'`, with the
  filters taken verbatim from `run_coverage.sh`'s `INVOCATION_MATRIX`. Listing is a registration
  query needing no binder driver, so B's and C's counts are measurable on a driverless host; they
  are measured rather than derived because arithmetic over the table misses a renamed or
  unclassified suite.
- Invocation A was executed on the host (599 selected from 23 suites, 599 passed, exit 0).
  B (459 of 459 passed) and C (415 of 415 passed) were executed in a binder-capable guest; a
  count mismatch there means a filter or classification drifted, not that a test failed.
- Invocation A's filter is written in the source block's pointer, because the fixtures' `SetUp`
  diagnostics and the L1 workflow (`.github/workflows/L1-tests.yml`) cite it from there; it
  equals `run_coverage.sh`'s `-${CONTRACT_AIDL_ONLY_SUITES}`. B's and C's filters are not written
  out: `run_coverage.sh` derives them from five classification constants (`NEUTRAL_SUITES`,
  `LEGACY_BOUND_SUITES`, `CONTRACT_ANY_BACKEND_SUITES`, `CONTRACT_LEGACY_ONLY_SUITES`,
  `CONTRACT_AIDL_ONLY_SUITES`) and assembles them per invocation in `INVOCATION_MATRIX`; a copy
  would be a second definition that drifts. Classification per fixture, which the manifest's
  Back-end column gives as any, legacy and AIDL: Compatibility, Preflight and LocalInstance →
  `CONTRACT_ANY_BACKEND_SUITES`; Selection and LegacyArm → `CONTRACT_LEGACY_ONLY_SUITES`; Session
  and Transmit → `CONTRACT_AIDL_ONLY_SUITES`. A fixture in none of those lists breaks the
  selected-plus-excluded reconciliation, so a new fixture needs a matching entry there. Every case
  in the file belongs to one of the seven fixtures, and all seven are classified.
- Invocation A's selection is negative (it excludes the two AIDL-only fixtures) and that is not
  optional: under A their `SetUp` fails loudly with a diagnostic naming the mode they need.
  `GTEST_SKIP` was rejected because a skipped arm is indistinguishable from a passing one in an
  aggregate count, the swallowed-failure shape the file exists to avoid; no case in the file
  skips on any invocation. `run_coverage.sh` passes a filter through via `GTEST_EXTRA_ARGS` and
  emits an advisory that the run was a partial selection.
- Invocation C selects only the three back-end-independent fixtures. `DriverAidlSelectionTest` is
  excluded because under mode `incompatible` the legacy back-end is selected for a different
  reason (a present, rejected service) and the mid-process registration case would register a
  second service under a taken name, which the harness's collision check
  (`failIfServiceAlreadyPublished()` in `tests/L1Tests/test_main.cpp`) forbids.
- `EXPECTED_SUITE_PATTERN` names twelve fixtures: the nine oldest, largest pre-existing ones and
  this file's three back-end-independent fixtures, `DriverAidlCompatibilityTest`,
  `DriverAidlPreflightTest` and `DriverAidlLocalInstanceTest`. The pattern only proves the
  results file came from this suite; a partial run is caught by the per-invocation count
  reconciliation, which is why widening it is safe, and that reconciliation runs before the name
  check, so a narrowed run is reported on its count. The count gate in `verify_results` reconciles
  the executed count against `--gtest_list_tests` rather than a hardcoded number, so added cases
  need no expected-count edit.
- Superseded: an earlier comment held that `EXPECTED_SUITE_PATTERN` needed no edit for this file
  and that none of its fixtures belonged in it, because a run exercising only this file is the
  partial run the pattern exists to catch; `run_coverage.sh` names the three fixtures above.
- References into `run_coverage.sh` are by name rather than line, because that file grows and a
  line citation into it stops identifying its subject; shell function and constant names are
  greppable and survive edits.
- Invocations D and E belong to `hdmicec/tests/L2Tests` (`test_main.cpp`,
  `ccec/test_DualPathIntegration.cpp`) and are not duplicated here, because an in-process fake is
  not IPC: libbinder resolves a name registered in the calling process to the local `BBinder`, so
  `interface_cast` returns that very object, no `Bp*` proxy is created, no transaction crosses the
  driver and the client threadpool is not involved. That loses real transport, which is why the L2
  tier hosts the fake in its own process and why the "callback arrives on a binder thread"
  evidence is invocation E's.
- It gains the reason B and C are in-process: `halcompat::isCompatible` reads metadata through
  `getInterfaceHash()` and `getInterfaceVersion()` (`halcompat.h:164`, `:171`), which on a local
  object dispatch virtually and can be overridden. A remote `Bn*` service cannot report bad
  metadata, because its generated `onTransact` answers from compiled-in constants, so the
  compatibility-rejection branches are reachable only in-process and invocation C has no L2
  counterpart.
- Invocation C is not redundant with the predicate's unit tests. "Present but incompatible falls
  back to legacy" is factory behaviour; the selection helper acting on the answer is
  `resolveBackEnd()` in `ccec/src/Driver.cpp`, in an anonymous namespace with internal linkage,
  so it cannot be called from a test. Only a process started with an incompatible service
  registered proves the factory acts on the predicate.
- Established by invocation A, on any host, with no binder driver and no service:
  - every compatibility arm by direct call: accept, accept-newer-same-major, and rejection on a
    null service, an empty hash, `"-1"`, `"notfrozen"`, an older same-major version, a
    cross-major version and a cross-era version;
  - every preflight arm through the probe seam: the empty path, an unopenable node, a node that
    opens but refuses the version ioctl, a node whose protocol version differs, a matching
    protocol whose context manager never answers, and the direct positive verdict, plus
    descriptor hygiene (one close per successful open) on all six;
  - the legacy halves of the authorized observable differences, and SC6(f)'s legacy half: the
    physical address is read through the legacy HAL call;
  - the AIDL back-end's closed-state guards, its `writeAsync` prelude ordering and its unguarded
    methods, on locally constructed instances;
  - that the factory returns the same object every time and, only where the environment can
    publish a service, that a service appearing mid-process does not change the resolved
    back-end; where it cannot publish, that case says so in its output and the SC6(c) evidence is
    not claimed;
  - that the two Sink call paths the file models still have the structure the model assumes, read
    from the real plugin source, which is a required input: a source that cannot be located fails
    the case, because a model nobody checked against its subject is not evidence about a caller;
  - the legacy back-end's receive path end to end, measured: a frame injected at the legacy HAL's
    Rx callback is delivered within a bounded wait, byte for byte, to an application
    `FrameListener` through the Bus reader and `Connection`. That is the baseline the AIDL receive
    assertions are compared against (the receive path is not an authorized difference), and it
    proves the observation machinery the AIDL cases use (`RecordingFrameListener`,
    `ListeningConnection`, the bounded wait) is correctly wired on a host where it can run.
- Registered here but executable only on a binder-capable runner (invocation B):
  - the adapter's translation both ways: one-element address marshalling for add, remove and get;
    the inverted ACK sense across directed and broadcast; frame-length policing; the non-ok-status
    mapping on every consumed method;
  - the session lifecycle: the duplicate-open no-op, the null-controller and non-ok-status open
    guards, the close sequence, which controller was closed, and that the close sentinel is
    offered before the transaction's result is evaluated, proved with a reader parked on the
    incoming queue;
  - the AIDL receive path: bounded observable delivery through the Bus reader to an application
    `FrameListener`, bounded non-delivery plus release while closed, and the post-detach drop
    after a failed close and after the owner's destruction. The machinery is exercised for real
    under invocation A by the legacy-arm receive case, so a failure here is about the AIDL
    back-end rather than the harness;
  - that a binder threadpool exists in the process, the only externally observable consequence of
    the obligation production's `open()` discharges;
  - the content of both diagnostic callbacks' reports, from the middleware's captured output:
    `onStateChanged`'s transition with both state names at `LOG_INFO`, and `onMessageSent`'s
    status name, message length and bytes at `LOG_DEBUG`, the level raised through production's
    `check_cec_log_status()` under `ScopedCecLogLevel`'s custody protocol, with the raise asserted
    so a refusal cannot pass as evidence.
- Not established by the file at any invocation:
  - real IPC: no case creates a `Bp*` proxy, crosses the binder driver or receives a callback on a
    threadpool thread; an in-process fake resolves to the local `BBinder` and its triggers run on
    the calling thread. That evidence is invocation E's
    (`hdmicec/tests/L2Tests/ccec/test_DualPathIntegration.cpp`), and the L1 threadpool assertion
    shows the pool's presence only, not its use;
  - the plugin's runtime behaviour: the Sink drift guard reads the plugin's source and asserts its
    call shape, failing when the source cannot be located, but does not compile, link or run the
    plugin (its own test binaries link the framework's CEC mock instead of this middleware), so
    what the plugin does with caught exceptions remains its own suite's and the L3 step's
    evidence.

### `kAidlMaxMessageLength`

- `ccec/src/DriverAidlImpl.cpp` defines `AIDL_MAX_MESSAGE_LENGTH` in an anonymous namespace, so
  restating it is the only option; the static assertion pinning the `CECFrame` capacity the
  conflict rests on keeps the restatement honest, so the numbers cannot drift apart unnoticed.

### `kSelectedBackEndLogFormat`

- The constant and the two back-end names after it live in an anonymous namespace in
  `ccec/src/Driver.cpp`, by design, since the string is defined once and referenced nowhere else.
  The copy is exactly what is there, and the case using it does not merely compare two local
  strings: it drives the production logger with this format and asserts the emitted line, so a
  reworded production string or a lowered default log level is visible.

### `kClientInterfaceVersion`

- `IHdmiCec::VERSION` is declared `static const int32_t VERSION = 1000;` (`IHdmiCec.h:25`) with
  an in-class initializer and no out-of-line definition anywhere in the snapshot (checked against
  the generated sources and the shipped `libhdmicec-v0.1.0.0-cpp.so`, which exports no such
  symbol). It is usable in a constant expression and by value, but every GoogleTest comparison
  macro binds both operands to a const reference, an odr-use, which fails to link with
  `undefined reference to com::rdk::hal::hdmicec::IHdmiCec::VERSION` (measured). The static
  assertion still reads the header member directly, because a constant expression pins the real
  value rather than the copy.

### `kFrozenInterfaceHash`, `kBrokenInterfaceHash`, `kUnfrozenInterfaceHash`

- `kFrozenInterfaceHash` is `IHdmiCec.h:27`. `HASHVALUE` needs none of the version's treatment
  (`static constexpr char*` is implicitly inline in C++17, so it has a definition) and is copied
  for symmetry, so every comparison names a local constant.
- `"-1"` is the hash halcompat rejects as a failed hash RPC (`halcompat.h:165-167`);
  `"notfrozen"` is the pre-freeze development hash (`halcompat.h:168-170`).

### `kControllerClientInterfaceVersion`, `kControllerFrozenInterfaceHash`

- `IHdmiCecController::VERSION` is also declared in-class with no out-of-line definition, so
  odr-using it in a comparison macro fails to link; the copy is what the fake-metadata cases
  compare the controller's reported version against. The hash (from `IHdmiCecController.h`)
  needs no such treatment and is copied for the same symmetry as `kFrozenInterfaceHash`.

### `kDivergentReportedVersion`, `kDivergentReportedHash`

- 4242 is deliberately not a halcompat table value: those select an arm of the compatibility
  rule, whereas this exists only to differ from the constant a fake would otherwise report, the
  condition its divergence trace fires on. A table value would suggest the metadata cases make a
  compatibility claim, and they do not.
- The hash is forty hex characters, the shape of a real hash rather than a sentinel: the fake
  stores whatever it is given, and a value looking like an error code would invite the reading
  that only error codes can be installed.

### `kOlderSameMajorClient`

- The pair exists only because the older-same-major arm is unreachable for this client
  (unreachable path 1), so it is reached with a different client; 3020 is era 0, major 3,
  minor 2.

### Compile-time invariants

- Each assertion pins a value a runtime expectation silently depends on, and each message says
  what to do if it fires, so a snapshot regeneration or header edit stops the build at the
  analysis it invalidated instead of quietly inverting a runtime expectation.
  `detail::isCompatible` is `constexpr` (`halcompat.h:108`) and `detail` is an accessible
  namespace, so the whole version table is checkable without running anything.

### `StdoutCapture`

- Reproduced from the established implementation in the same directory
  (`ccec/test_Util.cpp:143-211`) rather than invented, for the reasons recorded there: the code
  under test logs through `printf` (C stdio on fd 1), so redirecting the `std::cout` streambuf
  would capture nothing, and GoogleTest's `CaptureStdout` lives in `::testing::internal`, which
  upstream documents as outside the public API.
- Why the file needs it although the timing looks wrong: the selected-path line is emitted once,
  inside `LibCCEC::init` in the global environment's `SetUp`, before any test body runs, so no
  test can capture the original emission; it has gone to the process log, where
  `run_coverage.sh` greps for it. The case instead drives the production logger with the
  transcribed contract format and captures that into an anonymous temporary. That proves the
  format is one `CCEC_LOG` accepts and substitutes into; proves the line still passes the default
  log-level filter, so a lowered default that broke every consumer is caught; and keeps the
  re-emitted line out of the process log, which still carries exactly one selected-path hit, so
  the runner's grep contract is untouched.
- `tmpfile()` is used rather than a named path because it is unlinked as it is created, leaving
  nothing for another process to substitute.
- The full warning: keep an instance's lifetime to the narrowest scope that brackets the
  emission, and call `read()` (or let the destructor run) before writing anything that must reach
  the real stdout.
- Constructor: every step is failure-checked and unwound in place rather than reported by
  throwing, because a capture that cannot be established must leave stdout exactly as found;
  cases assert `isValid()` before relying on anything captured.
- Destructor: idempotent with `read()`, which performs the same restoration, so a case that reads
  the capture and one that abandons it both leave fd 1 where it started; nothing is thrown.
- `isValid()` returns false when construction could establish neither the redirection nor the
  saved descriptor, in which case nothing was redirected.
- `read()` restores the real stdout first, so nothing written afterwards is swallowed, then reads
  the temporary from the beginning; later calls return the same content.
- `restore()` is the single restoration path shared by `read()` and the destructor, which is what
  makes calling them in either order safe.

### `MetadataDouble`

- `halcompat::isCompatible<I>` takes a `const android::sp<I>&` and reads metadata through
  `getInterfaceHash()` and `getInterfaceVersion()` (`halcompat.h:164`, `:171`), both virtual. A
  local subclass of the snapshot's `IHdmiCecDefault` (`IHdmiCec.h:40-72`) overriding only those
  two is sufficient: no `Bn*` base, no service-manager registration, no `onTransact`. `IHdmiCec`
  derives from `::android::IInterface` and hence `RefBase`, so holding one in `android::sp<>` is
  valid.
- `IHdmiCecDefault` answers every interface method with `UNKNOWN_TRANSACTION`, which is right
  because the compatibility check calls none. Its stock metadata is an empty hash
  (`IHdmiCec.h:69-71`) and version 0 (`:66-68`), so the empty-hash arm needs no override and is
  asserted against the stock object deliberately, as evidence that the default really is empty.
- The full warning: a `Bn*` object could be registered and reached remotely, and its generated
  `onTransact` would answer the metadata transactions from compiled-in constants, making the
  overrides inert. Local virtual dispatch is the whole mechanism.
- The constructor takes the hash by value and moves it into place, so a caller may pass the frozen
  hash, `"-1"`, `"notfrozen"` or anything else an arm needs.

### `frozenDoubleReportingVersion()`

- The hash comes from the generated header rather than a literal, so a snapshot regeneration
  cannot leave a stale literal that would send every version case down the broken-hash arm
  (`halcompat.h:165-167`) and make them all pass for the wrong reason.

### `doubleReportingHash()`

- The version is the client's own, so a rejection can only have come from the hash; a double
  reporting both a bad hash and a bad version would pass its case while proving nothing about
  which arm fired.

### `temporaryDirectory()`

- `TMPDIR` is the environment's statement of where scratch files belong, and a run whose scratch
  space is a private per-run directory means it. It is validated, not trusted: it must be
  absolute, because a relative value would place the node relative to the working directory,
  which for this binary is inside the source tree, where a stray file shows up as untracked
  repository content; and it must exist, be a directory and be usable, or `mkstemp` would fail
  and the case would report a precondition failure unrelated to the code under test. Anything
  failing validation falls back to `/tmp`, the helper's former unconditional behaviour.

### `TemporaryNode`

- Used by the preflight case needing a path that opens but is not a binder driver. The node is a
  regular file, so the predicate refuses it at the character-device check (decision point 4)
  before the `BINDER_VERSION` ioctl is attempted; the node must be nameable for the duration of
  the call.
- The lifetime is RAII rather than a call, which is why it is a class: a manual unlink after the
  predicate returns is skipped by every path that does not reach it (a fatal assertion, an
  exception, a timeout that kills the process), each leaving a file in a frequently shared
  directory. The destructor runs on all of them but the last, and the last leaves at most one
  file in a directory the environment nominated for that purpose.

### `ScopedCecLogLevel`

- The middleware's level lives in `cec_log_level`, a file-static in `ccec/src/Util.cpp` with no
  setter. The only route to it is `check_cec_log_status()`, which reads a level name from the path
  production hardcodes (`fopen("/tmp/cec_log_enabled")` at `ccec/src/Util.cpp:82`) and maps it
  onto that static. Raising the level from a test therefore means writing production's own file
  and calling production's own reader; any other seam would be a production change. The guard
  writes that file, drives that reader, and hands back content, mode, owner, existence and the
  process-wide level through `restoreAndVerify()`.
- Restoration is checked and proved, not attempted. `restoreAndVerify()` runs on the case's
  normal path while the custody lock is held and verifies three things: that the publication
  succeeded, every syscall result taken; that the bytes at the path are the captured original,
  re-read `O_NOFOLLOW`, or that the path is absent again where the guard created it; and that the
  effective level is the one observed on entry, re-observed through the same probe. The third is
  not implied by the second: `check_cec_log_status()` returns early when the path cannot be opened
  and silently leaves the level alone when the first line matches nothing in production's table,
  so "the file is back" and "the level is back" are two facts. That is also why restoration asks
  for the entry level by name before publishing the original bytes, the ordering
  `UtilTest::TearDown()` in `tests/L1Tests/ccec/test_Util.cpp:636-653` uses for the same reason.
  A case asserts on what `restoreAndVerify()` returns.
- The destructor and the atexit hook are backstops for the fatal assertion or `exit()` that skips
  the explicit call, never the route a case relies on. A destructor cannot fail a test, so a suite
  whose only restoration is a destructor reports success for a run that left the host altered;
  `restoreAndVerify()` closes that shape, and the backstops report their own failures on stdout,
  their only reader.
- Threat model, which is why the class is long. The path is fixed by production and lives in a
  shared, world-writable directory, the classic unsafe-temporary shape: anyone on the host can
  plant a symlink at the name, swap it between two of the process's syscalls (CWE-59, CWE-367) or
  write the file concurrently, and a second `run_L1Tests` on the same host is not hypothetical
  because sibling clones share `/tmp`. A plain truncating `ofstream` write follows a planted link
  and truncates its target with the process's privileges, and a run dying between truncate and
  restore leaves the victim corrupted. The protocol is lock, validate, atomically replace: an
  exclusive advisory lock on a companion path taken before anything is captured; `lstat()`
  classification refusing anything but a regular file this user owns, confirmed against the
  opened descriptor so it cannot be swapped underneath; reads and writes that never follow a
  link; and a write to a fresh `O_EXCL` temporary in the same directory, `rename()`d over the
  path, so a planted link is replaced rather than written through. Restoration runs the same
  primitive in reverse under the still-held lock. The contract is the repository's own:
  `LogConfigGuard` in `tests/L1Tests/ccec/test_Util.cpp` owns the identical path for the identical
  reason, and the lock path is the same string, derived from the same configuration-path
  constant, so the two units genuinely exclude one another across processes.
- Why a test needs it: `onMessageSent` reports at `LOG_DEBUG`, which the default `LOG_INFO`
  suppresses. A case asserting on such a line without raising the level would fail for a reason
  that is not a defect or, worse, be written to assert nothing, so a listener reporting nothing
  would pass.
- Failure is reported, never thrown or guessed at. Every refusal (a lock another process holds, a
  symlink or foreign owner at the path, an oversized file that could not be reproduced faithfully,
  a failed atomic replace) leaves the filesystem as it was, leaves `isRaised()` false and records
  which it was in `failureReason()`. The call site asserts on `isRaised()` and streams that reason,
  so "another run holds the lock" and "the path is a symlink" reach the log as themselves.
- The full warning: the level is process-wide and the lock is per open file description, so a
  second instance in the same process is refused exactly as a second process would be.
- The class formerly also cited `check_cec_log_status()` and `failureReason()` as see-also
  targets.

### `ScopedCecLogLevel::ScopedCecLogLevel()`

- `levelName` comes from production's table (`ccec/src/Util.cpp:58-67`); a null or empty name,
  or one too long for the single-line configuration file, is refused rather than written.
- `isRaised()` is true only when the lock was taken, the path classified, the effective level
  observed (so restoration has a reference), the requested name written atomically, read back to
  confirm the bytes at the path are those written, and production's reader driven with them in
  place. In every refusal before publication nothing on disk changed. In the one refusal after
  publication, the read-back finding foreign bytes, the original is rolled back and the rollback
  verified by the same checks `restoreAndVerify()` applies; where the rollback cannot be proved,
  the custody lock is deliberately retained and the atexit backstop left armed so the destructor
  retries and reports, because a failed assertion in the calling case restores nothing and
  protects no other process. `failureReason()` distinguishes the three outcomes, and a case must
  assert on the result rather than treat suppressed output as an assertion that held.
- On success the exclusive lock is held until `restoreAndVerify()` succeeds or the object is
  destroyed. On a refusal it is released, except on the unproved-rollback path, where it is held
  to the destructor for the same reason it is held on success: something published is not yet
  proved back.
- The line is formatted first, into a fixed buffer, because a name that does not fit must be
  refused before the lock is taken and before anything is captured, when there is nothing to undo.
- Lock first, capture second: the path is fixed by production code, so a second writer is
  possible, and capturing before locking would capture whatever that writer had put there and
  later hand it back as the original.
- The level to restore is observed rather than derived from the captured bytes, and the two are
  not interchangeable: `check_cec_log_status()` returns early when the path cannot be opened
  (`ccec/src/Util.cpp:82-86`), so a host whose file is absent, or names a level production's table
  lacks, has an effective level the file does not describe; restoring to what the file said would
  move a level the guard never raised. It is observed inside the custody window, after the lock
  and before publication, so no lock-respecting writer can move it between observation and raise.
- On an `atomicWrite()` failure there is nothing to restore: `rename()` is the only publishing
  step and is all-or-nothing, so the path is as found and the temporary already unlinked.
- On a read-back mismatch, something outside the lock replaced the path in the interval. The
  object has modified the file, so the original goes back before the refusal is reported, and
  whether that succeeded is carried into the reason, because a refusal that also failed to restore
  is a worse condition than one that left the host as found. When the rollback is published and
  verified by the same two checks `restoreAndVerify()` uses, nothing is outstanding and custody
  goes; publishing without verifying would report the refusal while the host stayed altered.
  When it is unproved (publication or verification failed), releasing the lock and clearing the
  active pointer would disarm every backstop exactly when one is needed, so the lock stays held,
  the atexit hook stays armed and `restorePending_` stays true for the destructor to retry.

### `ScopedCecLogLevel::restoreAndVerify()`

- It is the only route that can tell a case anything, and it runs while the custody lock is held
  so the original returns before any other process can observe the raised value as the host's.
  Three verifications, each with its own silent failure mode:
  1. Publication: `restoreLevelAndPath()` names the entry level back through production's reader,
     then publishes the captured original or removes the file the guard created. Every step is
     checked, since a discarded write, rename or unlink leaves the shared file holding the raised
     level or leaves a file the host never had.
  2. Content or absence: the path is re-opened `O_RDONLY|O_NOFOLLOW` and compared with the
     original, or its absence confirmed by `lstat` reporting `ENOENT`. A rename that reported
     success against a path something has since replaced is caught here and nowhere else.
  3. The effective level: `probeEffectiveLevel()` runs again and is compared with the entry level,
     proving "the file went back and the level followed it". Step 1's ordering exists because
     `check_cec_log_status()` silently leaves `cec_log_level` alone when nothing matches
     (`ccec/src/Util.cpp:87-97`), so a byte-perfect restoration of a file naming no level would
     leave the raised level in force; step 1 asks for the level by name and step 3 proves it held.
- Custody is released and the atexit backstop disarmed only after all three hold; on failure the
  lock is kept and the backstop stays armed so the destructor still tries.
- `detail` names which verification failed and what was observed, so a publication fault, a
  foreign writer and a level that did not move are distinguishable rather than "restore failed".
- True also covers "nothing to take back": the guard refused to raise, or a previous call already
  restored. A second call returns true touching neither path nor level.
- The early return exists because nothing was published or it was already proved back, and a
  second call must not rewrite a path the guard no longer has custody of. `restorePending_` is
  tested separately from `raised_` because the constructor can publish and then fail its
  read-back, leaving the path modified while the level was never raised; that state must reach
  the work below, or the path most needing verified restoration would skip it.
- On success the active pointer is cleared first, so the atexit backstop finds nothing to do even
  if the release is the last thing that happens to the object.

### `ScopedCecLogLevel::verifyRestoredState()`

- Factored out because the constructor's rollback must run exactly the same checks. A rollback
  published but not verified is the defect it makes impossible to reintroduce: two verification
  implementations would leave one unexercised by the suite.

### `ScopedCecLogLevel::~ScopedCecLogLevel()`

- It exists for the fatal GoogleTest assertion, the thrown exception and the `exit()` that unwind
  past the explicit call. It is not the route a case relies on: a destructor cannot fail a test,
  so a suite whose only restoration is a destructor reports success for a run that left the host
  altered (a raised level in force, or the shared file changed or missing), the shape
  `restoreAndVerify()` closes. Every case that raises the level calls that and asserts on it.
- It uses the same single restoration primitive (entry level by name, then the file back byte for
  byte or removed) and then `verifyRestoredState()`. A failure cannot reach the test, so it is
  reported on stdout naming the path, step and errno, since an unreported failure is
  indistinguishable from a clean exit. The former comment's claim that the backstop "performs no
  verification" did not match the code, which calls `verifyRestoredState()`; it is not carried.
- Two states reach the backstop and mean the same thing to it, something published not proved
  back: `raised_` (a case that never called `restoreAndVerify()` or whose fatal assertion skipped
  it) and `restorePending_` (the constructor's read-back failure, where the path was written, the
  level never raised and the rollback unproved; that path keeps custody so this retry can happen).
- The lock is released unconditionally and last: a lock kept past this point would outlive its
  justification and refuse custody to every later case in the binary and every concurrent
  `run_L1Tests` on the host, worse than the failure the retry already reported.

### `ScopedCecLogLevel::isRaised()`, `failureReason()`, `entryLevel()`, `observeEffectiveLevel()`

- `isRaised()` is true only when custody was taken, the effective level observed, the name written
  atomically, the bytes read back from the path and production's reader driven with them in place;
  it becomes false again once `restoreAndVerify()` has proved the level back.
- `failureReason()` names the specific refusal (contended lock, symlink or foreign owner,
  unreadable or oversized file, failed atomic replace, or a concurrent writer), so an environment
  fault and a custody refusal are distinguishable in the log.
- `entryLevel()` is exposed so a case can assert that the level moved and came back rather than
  take the guard's word; it is the value `restoreAndVerify()` compares against.
- `observeEffectiveLevel()` is public because the evidence is worth more than the encapsulation:
  `cec_log_level` has no accessor, so the only observation is emitting at each level and seeing
  which survive, and a case unable to do that cannot tell "restored" from "said it restored". A
  caller inside a `StdoutCapture` would find the probe's own markers in its captured text.

### `ScopedCecLogLevel::kLockSuffix`

- `tests/L1Tests/ccec/test_Util.cpp` appends `.testlock` to its own copy of the configuration path
  rather than writing the lock name out, so the two units lock one file. Deriving it on both sides
  keeps that true; a hand-typed whole lock name is one edit away from two "exclusive" holders of
  two different files, worse than no lock because it looks like one.

### `ScopedCecLogLevel::kLevelNames`

- A transcription of production's table (`ccec/src/Util.cpp:58-67`) in the same order, the index
  being the level, which production also relies on since it stores the table's second column
  through `atoi` (`:93`). Restoration must name the level it wants back because
  `check_cec_log_status()` compares the file's first line against these names and leaves
  `cec_log_level` alone when none matches (`:87-97`), so publishing the original alone does not
  ask for a level. `tests/L1Tests/ccec/test_Util.cpp:118-127` carries the same transcription; both
  are transcriptions because production's table has internal linkage and no accessor.

### `ScopedCecLogLevel::kUnobservableLevel`

- It means the effective level could not be observed, a refusal rather than a level: the
  constructor declines to raise on it, because a raise whose restoration cannot be proved is the
  shape the class exists to close.

### `ScopedCecLogLevel::probeEffectiveLevel()`

- `cec_log_level` is a file-static in `ccec/src/Util.cpp:39` with no accessor, and `CCEC_LOG`
  emits only at or below it (`ccec/src/Util.cpp:116`), so the highest level that still emits is
  the configured level and walking down from `LOG_MAX - 1` reads it without a production change.
  The technique is the repository's own (`probeEffectiveLogLevel()` in
  `tests/L1Tests/ccec/test_Util.cpp:584-595`), reimplemented because that unit's helpers are
  file-local and this file must not include another test translation unit.
- Each emission goes into its own `StdoutCapture`, so the probe writes nothing to the process log:
  leaked markers would pollute the output the cases assert against, and `run_coverage.sh` greps
  that log for the selected-path line. The marker carries the level and the pid, so a line left by
  any other writer cannot be mistaken for the probe's own.

### `ScopedCecLogLevel::sLockPath_`, `sTempPath_`

- Static and pre-formatted because the restoration path also runs from the `std::atexit`
  backstop, where formatting through `std::string` would allocate during teardown. The temporary's
  name is distinct from the one `test_Util.cpp` uses, so even a backstop running in an odd order
  cannot have the two units writing one temporary.

### `ScopedCecLogLevel::acquireLock()`, `releaseLock()`

- The lock is over a companion path, not the configuration file: locking the file would mean
  holding a descriptor across the `rename()` that replaces it, leaving the lock on the replaced
  inode and excluding nobody. `flock()` rather than `fcntl()` because the lock is wanted for the
  descriptor's lifetime with no byte ranges and drops automatically if the process dies.
  `LOCK_NB` with a bounded retry, because blocking forever would turn a contended host into a
  hanging suite instead of a reporting one.
- The lock file is never unlinked: another waiter may already hold a descriptor to it, and
  removing it would let a third process create and lock a new inode, two "exclusive" holders of
  two different files.

### `ScopedCecLogLevel::captureCurrentState()`

- `fstat()` on the opened descriptor confirms the inode read is the one `lstat()` classified,
  which closes the window between the two calls (CWE-367) rather than narrowing it. A file larger
  than the capture buffer is refused because a truncated copy could not be restored faithfully,
  and leaving the host with a truncated configuration would be worse than not touching it.

### `ScopedCecLogLevel::atomicWrite()`

- Allocation-free by construction (only open, write, fchmod, fchown, close, rename and unlink),
  because the restoration path calls it and that path also runs from the atexit backstop. Because
  the temporary is in the same directory, the rename is a same-filesystem atomic replace, so a
  planted link at the destination is replaced rather than written through.
- The two metadata calls are not equivalent. `fchmod` must succeed: the descriptor was created by
  the process with `O_EXCL`, so the file is its own, and a failure would leave the restored file
  with permissions the host did not have, the silent mutation the guard exists to prevent.
  `fchown` is best effort: an unprivileged process cannot give a file away, so `EPERM` is normal
  whenever the file was owned by someone else, and failing on it would abort every unprivileged
  run for a condition no test can influence.
- The `fchown()` result is taken into a named local and discarded rather than cast away at the
  call because glibc declares `fchown()` `warn_unused_result`, and a bare `(void)` cast on the
  call expression does not satisfy that attribute under `g++ -Wall`, which would cost a
  `-Wunused-result` warning in a build required to be warning-clean. Discarding a named local does
  satisfy it and makes the decision visible; there is no remedy an unprivileged process could
  apply to `EPERM`.

### `ScopedCecLogLevel::pathHolds()`

- It reads one byte past the compared length because a file starting with the data and continuing
  is not the file that was published, so the comparison must be able to see the extra byte.
- The buffer is one byte larger than the capture buffer because both publications are verified
  here: the short level line on the way in and the captured original, up to `sizeof(saved_)`
  bytes, on the way out. A buffer sized for the level line would report every longer original as a
  mismatch, a false restoration failure on any host whose configuration file is not a short line.

### `ScopedCecLogLevel::restoreLevelAndPath()`

- Used by `restoreAndVerify()` and both backstops, so there is exactly one implementation of "put
  it back" and no route can diverge from it.
- The order is the repository's own (`UtilTest::TearDown()` in
  `tests/L1Tests/ccec/test_Util.cpp:636-653` restores the level, then the bytes). Asking for a
  level means writing the configuration file, so:
  1. The entry level is named back by publishing the name production's reader matches for it and
     driving the reader. That is what moves `cec_log_level`, and it is not interchangeable with
     publishing the original: an empty file, or one whose first line matches nothing, leaves the
     reader holding the raised level (`ccec/src/Util.cpp:87-97`), so a host with such a file would
     be left at DEBUG by a faithful byte restoration.
  2. The captured original is published, or the created file removed, and the reader is not
     driven again, so the file ends as the host had it while the level stays where step 1 put it;
     driving the reader after step 2 would undo it.
- Every outcome is returned rather than discarded: a failed write, rename or unlink leaves the
  host altered, and a primitive that swallowed them would let that happen while every case
  passed. The level line is formatted into a stack buffer and every returned description is a
  static literal; `restoreErrno_` lets a report name the fault without re-reading an errno later
  steps have overwritten.

### `ScopedCecLogLevel::pathIsAbsent()`

- `lstat` rather than `stat`, as everywhere in the class, so a link at the name is reported as
  something present rather than as the absence of its target.

### `ScopedCecLogLevel::reportBackstopFailure()`

- The backstops run from a destructor and an atexit handler, neither of which can fail a test:
  GoogleTest's assertion macros need a live test to attribute a failure to, and an atexit handler
  has none. The run log is the only report available, so the line names the path, the step and the
  errno, letting a reader of a run that left the host altered see it said rather than infer it from
  a later case's verbosity.

### `ScopedCecLogLevel::atExitRestore()`

- atexit handlers run on return from `main()` and on `exit()`, normal control flow, so unlike a
  signal handler this may touch object state and the filesystem. It covers the exit that never
  reaches the destructor; a live `sActive_` means an instance still holds custody, since the
  destructor and `restoreAndVerify()` both clear it.
- No signal handler is installed deliberately: a fatal signal mid-rename would give two racing
  writers over one temporary, and corrupting the protected file is worse than leaving a
  debug-logging switch at a stale value a human can delete.
- It is best effort and not a substitute for `restoreAndVerify()`, the only route that can fail a
  case. It treats both states as the destructor does; one looking only at `raised_` would skip
  the state whose reason for keeping custody was to have a backstop retry it.

### `ScopedCecLogLevel` data members

- `restorePending_` is independent of `raised_` because the constructor can write the requested
  level and then fail its read-back, leaving the path modified while the level was never raised,
  a state `raised_` cannot express and the one in which abandoning custody would leave the shared
  path altered with every backstop disarmed.
- A raised guard always holds `entryLevel_` in `0 .. LOG_MAX - 1`, because the constructor
  declines the raise otherwise.
- `restoreErrno_` is captured at the step rather than read at the report because
  `check_cec_log_status()` and the following unlink both overwrite errno, so a later read would
  name the wrong fault; it is an `int` so the backstop can report it without allocating.
- `saved_` is a fixed buffer rather than a `std::string` because restoration must not allocate,
  running from the atexit backstop too. A larger file is refused outright rather than captured
  short (see `captureCurrentState()`).

## tests/L1Tests/ccec/test_DriverAidl.cpp (part 2 of 6)

### lowercaseHexOf

- Kept as one helper rather than written out at each call site, so a case asserting on the production `onMessageSent` log line compares against one rendering instead of a hand-typed literal that could drift from the message it was built from.
- Deliberate difference from production: `DriverAidlImpl::EventListener::onMessageSent()` renders into a stack buffer sized from `CECFrame::MAX_LENGTH` and appends an ellipsis to a longer message, whereas this helper is unbounded.
- The cases only pass short frames, two orders of magnitude inside that bound, so the two renderings coincide. A case that needed the truncating arm would have to assert against the bounded form; production refuses to send such a frame anyway (the frame-length policing asserted by `DriverAidlTransmitTest`), so only a misbehaving HAL could echo one back.

### Synthetic binder preflight probe (section comment)

- Three outcomes of the preflight's eight decision points cannot be reached from a path and a timeout on any host: a protocol-version mismatch (decision point 7) needs a node that answers `BINDER_VERSION` with a value other than the one compiled against; a silent context manager (decision point 8) needs a working driver whose context manager never registered; the positive verdict needs a binder-capable kernel.
- An absent path stops at the open (decision point 2) and a regular file at the character-device check (decision point 4), before the protocol read, so no filesystem arrangement short of a binder driver gets past decision point 6.
- `DriverAidlImpl::isBinderPreflightOk()` therefore takes its six kernel-facing operations — `openNode`, `identifyDescriptor`, `identifyPath`, `readProtocolVersion`, `pingContextManager`, `closeNode` — as a `BinderPreflightProbe` of plain function pointers, defaulted to `defaultBinderProbe()`.
- The same probe serves the pre-lookup custody re-verification, which is why `identifyPath` is among them: the re-verification resolves the name again, whereas the preflight asks its questions of the descriptor it already holds.
- Substitution makes every arm reachable on this host deterministically and without a binder driver, and lets the cases assert descriptor hygiene, which no black-box test of the predicate could observe.
- The state is file-scope because the probe members are plain function pointers: they carry no captured context, so a stateful lambda cannot convert to one and configuration and counters must live where the free functions can reach them.
- This is not a shared-state order dependency: GoogleTest runs cases on one thread, every case calls `resetSyntheticProbe()` before configuring anything, and nothing outside `DriverAidlPreflightTest` touches the state. Every production call site takes the default argument, `defaultBinderProbe()`, so production selection never sees it.

### kSyntheticBinderFd

- Chosen so that "the descriptor that was closed is the one that was opened" cannot be satisfied by accident.
- Nothing performs a real operation on it: every probe member is substituted (the probe has six members; an earlier comment said four), so the number is only passed between them.

### kSyntheticSecondBinderFd

- `isServiceAvailable()` opens the node twice: once through the preflight, whose descriptor it retains, and once for the pre-lookup liveness check.
- The liveness check cannot reuse the retained descriptor because the binder driver permits one mapping per open descriptor.
- A distinct number lets a case assert which descriptor was pinged and which released, rather than counting anonymous calls.

### kValidatedNodeIdentity

- What production does with device, inode and rdev is compare them, so what matters is that the "same node" and "different node" answers differ in exactly the field under test.
- Mode and owner are built from POSIX macros rather than from the production constants so the two are independent and a wrong constant cannot agree with itself.

### kSubstitutedNodeIdentity

- Only `inode` differs, deliberately: a substitution that changed every field would pass a comparison that looked at only one of them, whereas this one fails only if the inode is genuinely part of the comparison.

### kRePermissionedNodeIdentity

- Expresses a separate attack from `kSubstitutedNodeIdentity`, so it is a separate fixture rather than another variant: device, inode and rdev are identical, so this is the very node the preflight validated, `chmod`ed after validation.
- An adversary who cannot replace the node can still widen access to it, and a re-verification comparing only `(device, inode, rdev)` reports that as unchanged because the object genuinely is the same.
- The mode is world-writable rather than merely different, so the case demonstrates a privilege widening, not an arbitrary edit.

### kReOwnedNodeIdentity

- The other half of the same window and not implied by the mode case: `chmod` and `chown` are different operations with different preconditions, and a comparison could plausibly cover one and not the other.
- Device, inode, rdev and mode are identical, so a decline can only come from the owner being compared.
- The owner is an unprivileged uid, making it a handover of the driver node rather than a cosmetic change. The preflight's own root-owner check cannot catch it, because that check ran before the change.

### kWorldWritableNodeIdentity

- Not a malformed node: a binder device node has to be openable by every client process that uses binder, so AOSP-derived layouts publish it with mode 0666 deliberately.
- It is the regression guard against the one change in this area that would look like a security improvement and would in fact decline the AIDL path on every conformant production platform.

### SyntheticProbeState

- The `*Second` members exist because `isServiceAvailable()` asks twice: it pings the context manager once in the preflight and again immediately before the lookup, and identifies a descriptor once for the retained node and once for the reopened one.
- Arms where the platform changes between the check and the use are only reachable if the probe can answer differently on the second call. Each `has*Second` flag defaults to false, which keeps every case that does not opt in unchanged.
- `hasSecondDescriptorIdentifyResult` is distinct from `hasSecondDescriptorIdentity`, which changes the identity a successful call reports. The re-verification declines when the reopened descriptor cannot be identified and when it identifies as a different node; neither implies the other.
- `answerIdentifyDescriptor` cannot express the second-call failure because it applies to every call, and the preflight's own first call has to succeed for the second one to happen.

### syntheticOpenNode

- The second open answers a different descriptor so a case can tell the retained node from the one reopened for the pre-lookup liveness check.

### syntheticPingContextManager

- A differing second answer is the only way to reach the arm where the context manager answered the preflight and has stopped answering by the time the lookup is about to happen.
- The earlier `@return` text ("`answerPing`, unmodified") did not account for the second-call answer; the condensed comment describes the configured answer for each call.

### resetSyntheticProbe

- The identity answers reset to a well-formed node — a root-owned character device whose path and descriptor agree — so every case written before the identity arms existed keeps reaching the decision point it was written for.
- A case about an identity arm overrides the field it is about after calling this, which makes each of those cases name exactly one deviation from a good node.

### syntheticProbe

- `BinderPreflightProbe` is an aggregate of six function pointers, so this is a brace initialization and nothing more; the returned object binds to the predicate's const reference parameter for the duration of the call.
- Because the production code does not defend against a partially filled probe, this is the only aggregate initialization of the probe in the suite.

### frameOfLength

- The header and opcode are real so the frame is well-formed at every length; the padding carries no meaning.

### Sink caller drift guard (section comment)

- The case measuring the `addLogicalAddress` failure mapping through the Sink's two call paths models their exception handling: a `try` with three catches for one, an uncaught propagation for the other.
- A model is evidence only while it still describes the thing modelled, and nothing in a hand-written model notices when the plugin is reformatted, a catch arm is removed, or the caller stops calling the middleware.
- So the structure is read from the plugin source rather than restated. The plugin belongs to another component and is not modified, compiled or linked here (its test binaries link the framework's CEC mock, not this middleware); what is available and needed is its text: the two call sites, whether each sits inside a `try`, and which handlers that `try` carries.
- The scanner is deliberately not a regex: "is this call inside a try block" is a question about brace nesting, which pattern matching cannot answer, and a naive search would be fooled by a brace in a comment or string literal.
- It walks the file once, skipping line comments, block comments, string and character literals, maintaining the block stack and recording each try block's span with the catch clauses that follow. It is not a C++ parser and needs only braces, comments, literals and two keywords.
- Not handled: raw string literals (`R"(...)"`), which the scanned file does not contain, and macro-generated braces, which would confuse any textual approach. If the plugin adopts either, the guard reports a mismatch and the model must be re-derived by hand — a loud, visible outcome.

### CppSourceModel

- The model answers exactly the two questions the drift guard asks and no more.

### kSinkCallerSourceVariable

- The checkout the variable names is a required step in both CI workflows, at the reviewed commit recorded above the drift guard, so an absent or unreadable value is a fault; the guard's two failure arms report it.

### readWholeFile

- Opened binary so no newline translation occurs and the text the scanner walks is exactly what is on disk.
- An empty file counts as failure because the guard's only use for the text is to scan it.

### executableDirectory

- An empty result makes the executable-relative candidates unusable, which the guard reports rather than treating as a mismatch.

### locateAndReadSinkCallerSource

- Both CI workflows export `CEC_SINK_CALLER_SOURCE` only after checking the file exists, so an unreadable value means the environment misreports what it provided.
- The caller fails on that, and fails separately when nothing pointed at the source and no candidate resolved. The source is a required input, so both outcomes are failures, reported apart because they call for different fixes.
- Fallback anchors: the working directory, which is `hdmicec/tests/L1Tests` under `make check` and under `run_coverage.sh`; and the executable's directory, which is that same directory or its `.libs` subdirectory when libtool wraps the binary. Both depths were verified against the built runner's location.
- A result whose `found` is false still carries the path list, which lets the guard report where it looked instead of only that it failed.

### Receive-path observation (section comment)

- A receive-path case asserting only "the trigger returned true" would pass against an adapter that dropped every frame: the trigger's return value says whether a listener was captured, not whether the frame went anywhere.
- The delivery is what is observed: the adapter offers the frame onto the incoming queue, the Bus reader thread drains it, Connection's address filter admits it, and the `FrameListener` is notified.
- Delivery is asynchronous to the case body even when the callback runs on the calling thread, because the Bus reader is a separate thread.
- A condition variable with a deadline is used rather than a sleep: a sleep long enough to be reliable is wasted on every passing run, one short enough not to be wasted is flaky, and neither is an assertion. The idiom follows `ccec/test_Connection.cpp`.

### kFrameDeliveryTimeoutMs

- The wait returns the instant the frame arrives, so a passing case pays the real delivery latency and nothing more.
- Sharing the bound with the `ccec/test_Connection.cpp` end-to-end cases means a slow host does not make one of them flaky while the other holds.

### kNonDeliveryWindowMs

- Necessarily a real wait, since "has not arrived" and "will not arrive" cannot be distinguished instantaneously.
- Much shorter than the positive bound for two reasons: a correct rejection is synchronous with the callback, so a wrongly accepted frame would already be queued when the offer returns; and the driver is closed throughout the window, leaving the Bus reader spinning on its own state guard, so a long window buys no confidence and costs real CPU.
- The strong evidence is the assertion that the frame does not surface after a later re-open, which is an ordering, not a timing.

### kSlowHalCallWarnMs

- Restated rather than read because `SLOW_HAL_CALL_WARN_MS` sits inside the anonymous namespace of `ccec/src/DriverAidlImpl.cpp` (internal linkage; `nm` finds no dynamic symbol), and exposing it through the class would be a production change this suite should not make.
- The only case using it delays a HAL call by this value plus a margin and asserts that the warning line appears. If production raised its threshold above this value, no line would be emitted and the case would fail — a loud wrong answer. If production lowered it, the case still crosses and still passes.

### RecordingFrameListener::frameAt

- An empty vector is not distinguishable from a zero-length frame, which nothing in the suite produces.

### StallingFrameListener

- The recording listener cannot make the incoming queue fill: the Bus reader is normally blocked in `EventQueue::poll()` on an empty queue and woken by each offer, so delivering many frames never shows an occupancy above one and the queue's refusal arm stays unreachable.
- The mechanism is the reader's own call graph rather than anything injected into the queue: the reader takes a frame and notifies each registered listener on its own thread, and a notification that does not return leaves the reader inside it rather than back at `poll()`. Frames offered from then on accumulate — the stalled-reader condition the middleware's refusal arm exists to survive.
- A case that forgets `release()` hangs its own teardown; the cases release before their assertions so an early-returning `ASSERT_*` cannot strand the reader.
- All state is `mutable` because `FrameListener::notify()` is `const`, the same accommodation `RecordingFrameListener` makes.

### StallingFrameListener::notify

- The delivered content is asserted by a recording listener attached alongside where it matters.
- Arrival is recorded before blocking and waiters are notified, so `waitUntilParked()` cannot race ahead of the block.

### kBinderThreadNamePrefix

- In the pinned Binder SDK, `androidCreateRawThreadEtc()` takes its `threadName` parameter as `__android_unused`, and the block that would honour it — together with `thread_data_t::trampoline`, the only site calling `androidSetThreadName()` for a spawned thread — sits inside `#if defined(__ANDROID__)`, which the SDK's CMake build never defines. `prctl(PR_SET_NAME)` is never reached and each pool thread inherits the process's own `comm`.
- Measured: a guest with a live binder driver, an AIDL back-end open since `LibCCEC::init` and a pool demonstrably started had no thread matching the prefix, and an assertion resting on it failed against correct production code. Every thread of the runner, and of the fake service host (which also starts a pool), reports its process's own name.
- `ProcessState::getThreadPoolMaxThreadCount()` reads the flag `startThreadPool()` sets and behaves the same on both platforms, so the pool is asserted through it. The prefix shows what a build that did apply the name would display, so the probe is kept as corroboration and reported rather than asserted.

### processHasABinderThread

- `/proc/self/task` lists one directory per thread; each thread's `comm` is compared against the prefix `ProcessState::makeBinderThreadName()` composes.
- A false result is not evidence of an absent pool for two reasons, neither recoverable in-process: a host without `/proc` reports nothing (which `procIsReadable` distinguishes), and on this port the name is never applied.
- Counting threads is not a substitute: the runner's thread count moves as cases start and join their own workers (measured going 1 to 5 to 6, with different thread ids at equal counts), so a count-based inference would be unsound.
- There is no reliable in-process observation that the pool started on this SDK. The evidence is invocation E's, which receives a `oneway` callback on a pool thread over real out-of-process IPC (`tests/L2Tests/ccec/test_DualPathIntegration.cpp`).
- Retained because a build that applies the name, or an SDK that switches to `pthread_setname_np`, would report true; the caller prints what it observed either way.

### BlockedReaderState

- The parking case's point is that the reader may fail to be released. A thread that cannot be joined must be abandoned rather than allowed to hang the run, which is only safe if everything it touches outlives the scope that started it — hence the shared pointer rather than the case's stack.
- The waiting side is a bounded condition-variable wait, never a sleep.

### ListeningConnection

- A fatal assertion returns from the body immediately; a listener left registered would be a Bus entry pointing at a destroyed stack object, which the next case's frame would dereference. The destructor makes that unreachable however the case leaves.

### DriverAidlCompatibilityTest

- Most cases call `halcompat::isCompatible<IHdmiCec>()` against a locally constructed double; others drive the production rejection diagnostic directly; the last four drive the fake service's and fake controller's interface-metadata controls — the inputs every compatibility arm is arranged with — against locally constructed fakes.
- Nothing is ever registered with the service manager, as a hard constraint: registering a name calls `defaultServiceManager()`, which opens the binder driver, and on the pinned stack that aborts the process where the driver node is absent, as on the host. Local construction suffices because the metadata controls take effect only under local dispatch.
- No driver precondition is established, a deliberate departure from the fixture idiom of the driver-facing suites: those open the process-global driver in `SetUp` because their cases assert against it, while this fixture never touches the driver, the HAL mock or the shared library, and touching shared state for no reason is how an order dependency is introduced. `TearDown` is empty because nothing needs restoring.
- Self-sufficiency: every case constructs its own double, calls the predicate and asserts the result; nothing is carried between cases or read/written outside the fixture, and each case passes identically alone under `--gtest_filter` and inside the full suite, in any order.

### DriverAidlCompatibilityTest.TheCompatibilityDoubleAnswersNoInterfaceMethod

- This case establishes that the double is a legitimate stand-in rather than assuming it.
- `MetadataDouble` overrides only `getInterfaceHash()` and `getInterfaceVersion()`, which suffices because `halcompat::isCompatible` reads nothing else (`halcompat.h:161-171`).
- The risk in so narrow a double is that an interface method might be reached and quietly answer something plausible, letting a case pass for a reason it does not name. `IHdmiCecDefault` answers every interface method with `UNKNOWN_TRANSACTION` (`IHdmiCec.h:45-65`), so such a call surfaces as a non-ok status; the case asserts this on all seven methods so the guarantee is measured rather than read off the header.
- `onAsBinder()` returning nullptr is why the fake service derives from `BnHdmiCec` rather than `IHdmiCecDefault` (recorded on `FakeHdmiCecService`): an object derived from the default can never be published to the service manager nor reached through a proxy. Pinning it here, where the suite's one `IHdmiCecDefault`-derived object lives, keeps the reasoning attached to what it explains.
- The double is held by its concrete type because `IInterface` declares `onAsBinder()` protected and `IHdmiCecDefault` re-declares its override public; the other calls would work through `sp<IHdmiCec>` too.

### DriverAidlCompatibilityTest.IncompatibleWhenServiceIsNull

- Rejected at `halcompat.h:161-163`. Because selection takes this arm whenever the service manager has nothing published under the production name, it is the most frequently exercised branch in production and the cheapest to get wrong by assuming a null check exists further down.

### DriverAidlCompatibilityTest.IncompatibleWhenInterfaceHashIsEmpty

- An empty hash means the hash query itself failed — a broken link rather than a development build — and is rejected at `halcompat.h:165-167`.
- The subject is the stock `IHdmiCecDefault`, unmodified, because `IHdmiCec.h:69-71` already returns `""` from `getInterfaceHash()`. Asserting against the stock object demonstrates that the snapshot's own default really is the rejected value. The case asserts the empty hash first, then that `isCompatible` reports false.

### DriverAidlCompatibilityTest.IncompatibleWhenInterfaceHashIsMinusOne

- `"-1"` is the other spelling of a failed hash query, rejected by the same condition at `halcompat.h:165-167`.
- Not interchangeable with the empty case: it is the exact value the L1 harness installs for invocation C (its `BROKEN_INTERFACE_HASH`, applied by `publishFakeForMode()`).
- The double's version is asserted equal to this client's first, so the rejection can only come from the hash.

### DriverAidlCompatibilityTest.UnfrozenServerIsRejectedByDefaultAndAcceptedOnlyWhenOptedIn

- A `"notfrozen"` hash is a pre-freeze development server, which makes no compatibility promise. `halcompat.h:168-170` returns `allowUnfrozen` for it, defaulting to false (`halcompat.h:159`), so the default-argument call production makes from `DriverAidlImpl::isServiceAvailable()` rejects it.
- Asserting rejection alone could not distinguish "rejected because unfrozen servers are opt-in" from "rejected because the hash was not recognised at all".

### DriverAidlCompatibilityTest.CompatibleWhenServerReportsThisClientsVersion

- If this identity case failed, every rejection case would be meaningless because nothing would ever be accepted.
- The real frozen hash is asserted first because otherwise the hash arm could reject the double and the case would pass without reaching the version comparison.

### DriverAidlCompatibilityTest.CompatibleWhenServerReportsNewerVersionInSameMajor

- The era-0 rule at `halcompat.h:112-114` accepts any server that is same-era, same-major and not older, so 1010 against 1000 must be accepted.
- A test treating "not exactly our version" as incompatible would look reasonable and pass against a fake reporting the exact version, while asserting a rule that does not exist and would reject a legitimately upgraded HAL in the field.

### DriverAidlCompatibilityTest.IncompatibleWhenServerReportsDifferentMajor

- A different major generation is breaking in era 0 and is rejected by the major equality conjunct at `halcompat.h:113`, even though 2000 is numerically larger than this client's 1000.

### DriverAidlCompatibilityTest.IncompatibleWhenServerReportsDifferentEra

- Rejected by the era equality conjunct at `halcompat.h:112`, a third distinct reason. 100000 has era 1 and major 0, so it fails on era first, not on the cross-major branch.

## tests/L1Tests/ccec/test_DriverAidl.cpp (part 3 of 6)

### DriverAidlCompatibilityTest.IncompatibleWhenServerReportsUnfrozenGeneratorVersion

- Version 1 is the generator's default: what a pre-freeze development server reports through
  the version channel rather than through the hash.
- It decodes to era 0, major 0, so the cross-major conjunct rejects it. The available mistake
  is to label it "older same-major", since 1 is smaller than 1000; it is not, and no
  older-same-major value exists for this client (the file header's unreachable path (1)).
- Evidence: the decoded major of 1 is asserted to be 0 and to differ from this client's, which
  pins the case to the cross-major conjunct.

### DriverAidlCompatibilityTest.OlderSameMajorServerIsRejectedByTheOrderingRule

- Exercises the era-0 ordering conjunct of `halcompat::detail::isCompatible` (`halcompat.h:114`
  when written), the executable form of unreachable path (1).
- For client 1000 the same-era, same-major servers are exactly 1000 through 1999, and each
  satisfies server >= client, so the ordering conjunct can never be the one that rejects.
  Calling `detail::isCompatible` directly with client 3020 and server 3000 (era 0 == era 0,
  major 3 == major 3, 3000 >= 3020 false) reaches it.
- The two preceding assertions establish that the era and major conjuncts hold, which makes
  this a proof about the ordering conjunct rather than another rejection. The reversed pair is
  the positive control: a newer server in that major is accepted.
- halcompat's own `static_assert(!isCompatible(3000, 2000), "era0 older rejected")`
  (`halcompat.h:128` when written) does not reach this arm: major(3000) is 3 and major(2000) is
  2, so that pair is rejected on major and only re-covers the cross-major case.

### RecoveringMetadataDouble

- Models the generated proxy's caching rule, the one behaviour
  `DriverAidlImpl::isServiceAvailable()` must never assume away: a metadata value read
  successfully is cached, but a failed read is stored as -1 and retried on the next call. A
  transaction that failed for the compatibility decision can therefore succeed for a
  diagnostic reread taken immediately afterwards, and the two calls disagree.
- Not derived from `BnHdmiCec`, for the reason `MetadataDouble`'s warning gives: a remote `Bn*`
  answers metadata transactions from compiled-in constants, which would make the override
  inert.
- `getInterfaceHash()` returns `kBrokenInterfaceHash` ("-1") on read 1 and
  `kFrozenInterfaceHash` afterwards. `hashReads()` is asserted on because it attributes a
  disagreement between two compatibility calls to the modelled first-read failure rather than
  to some other difference between the calls.

### DriverAidlCompatibilityTest.ARecoveredMetadataReadMakesTwoCompatibilityCallsDisagree

- The counterexample that motivated the current design, kept as the model of the hazard: a
  compatibility decision and any later look at the server's metadata are separate
  transactions. With a first read that fails and a second that recovers, halcompat rejects on
  read 1 and read 2 reports a perfectly compatible server.
- Two diagnostic shapes are unconstructible because of it:
  - A diagnostic that re-read the metadata and selected a cause from the reread hash would
    fall through its whole chain (a recovered hash is neither empty, nor "-1", nor
    "notfrozen") and announce that the version violated the era-and-major rule, when the
    version was never the problem.
  - A diagnostic that asked halcompat a second time and then read hash and version separately
    would combine a predicate retry that still failed with a metadata retry that succeeded,
    land in its not-compatible arm holding compatible values, and blame the version rule.
- The rule that forecloses both: one deciding predicate call, after which
  `DriverAidlImpl::emitCompatibilityRejectionDiagnostic()` takes exactly one snapshot and
  reports it as evidence about the server while declining to say which rule applied. This case
  models the instability; later cases drive the real emitter and assert its wording.
- Evidence: the first call rejects, the second accepts, and the double reports exactly two
  hash reads, so the disagreement is attributable to the modelled first-read failure alone.

### ThrowingMetadataDouble

- Models the one remaining shape the production diagnostic must survive: the snapshot read
  itself failing. Over binder that is a dead or refusing service; here it is an exception from
  a direct call, which is what the emitter's own `catch(...)` sees either way.
- The case using it shows a failed observation costs a less specific message and cannot
  disturb the cause the caller already recorded.

### DelayedRecoveryMetadataDouble

- Models delayed recovery: a diagnostic that asked the deciding predicate a second time and
  then read hash and version again separately could combine a second read that still failed
  with a third that succeeded, and so report compatible values from its not-compatible arm.
- The single-snapshot rule `DriverAidlImpl::emitCompatibilityRejectionDiagnostic()` follows
  makes that combination unconstructible; this double holds the rule to account.
- `hashReads()` pins the recovery to the third read rather than an earlier one.

### DriverAidlCompatibilityTest.TheObservedHashDescriptionNamesEachCategoryWithoutClaimingCause

- The observation helpers are tested directly because that is the only way to cover them on
  this host: `isServiceAvailable()` reaches its compatibility stage only after
  `isBinderPreflightOk()` passes on the default driver path, and with no binder transport the
  production arm emitting this wording is unreachable in-process (it belongs to invocation C,
  which is deferred on the host). The two helpers are public statics for exactly this reason.
- The classifier needs no service and no resolved back-end.
- Evidence: the four categories each yield their own phrase, and every phrase is asserted free
  of "because": a post-decision read is not entitled to say why the decision went the way it
  did.

### DriverAidlCompatibilityTest.AnObservedSnapshotThatWouldBeAcceptedReportsChangedMetadata

- A snapshot that would itself be accepted must never be blamed on the version rule. The
  production message depends on this invariant: when the predicate is true the message says
  the metadata changed or recovered; when false it names all three possible causes and
  attributes none.
- Evidence: the frozen digest with this client's own version is accepted, and so is the same
  digest with a newer version in the same era and major. A helper treating "not equal" as
  unacceptable would encode a rule that does not exist.

### DriverAidlCompatibilityTest.AnObservedSnapshotIsNotAcceptedForABadHashOrAnIncompatibleVersion

- Neither half of an observed snapshot can be waved through.
- Evidence: each non-digest hash is paired with a compatible version so only the hash can
  reject; the frozen digest is paired with an older-same-major, a cross-major and a cross-era
  version so only the version can. That shows the version half delegates to halcompat rather
  than reimplementing it.

### Production-emitter section (cases driving emitCompatibilityRejectionDiagnostic)

- The verifier's exact sequence, reproduced: fail, fail, then succeed. The historical hazard:
  the decision read fails; a second predicate call retries and fails, so the code concludes
  "still not compatible"; a third, separate read succeeds and returns compatible values; the
  classification runs on those and blames the version rule. Production now takes one snapshot
  and asks the predicate about that snapshot alone, so the halves cannot be mixed, and
  whichever read the snapshot lands on, the reported wording is a property of that one pair.
- The earlier cases test the observation helpers, which left the emitted wording untested. The
  section's cases call `DriverAidlImpl::emitCompatibilityRejectionDiagnostic()`, the only
  production emitter (called from `isServiceAvailable()` and nowhere else), and assert on the
  text it writes; no second copy of the message exists in the test to drift from it.
- Why the emitter rather than `isServiceAvailable()`: the latter reaches its compatibility
  stage only after the preflight passes on the default driver path, which this host's lack of
  binder transport prevents; that belongs to invocation C, which remains what proves the
  factory reaches the emitter.

### DriverAidlCompatibilityTest.TheProductionDiagnosticNamesAllThreeRulesAndClaimsNoneOfThem

- Evidence: the captured text mentions each of the three rules and attributes none of them.
- The observed-values assertion is not redundant: the first version of this message was about
  900 characters and `CCEC_LOG` truncates at 499 (`MAX_LOG_BUFF` in `ccec/src/Util.cpp`), so
  the qualification survived and every value the diagnostic exists to report was cut off.
  Capturing the output is what exposed it.

### DriverAidlCompatibilityTest.TheProductionDiagnosticBlamesChangedMetadataNotTheVersionRule

- Evidence: the captured text names the changed-or-recovered metadata and does not name the
  version rule, the misattribution a re-reading diagnostic would make.
- One decision then one snapshot is the sequence in which a diagnostic that re-read or
  re-asked would misreport the cause.
- The message names the version rule as one of three possibilities, which is why the guard
  asserts on the causal claim rather than on the phrase.

### DriverAidlCompatibilityTest.DelayedMetadataRecoveryOnAThirdReadCannotBeBlamedOnTheVersionRule

- Evidence: the captured text is the same changed-or-recovered wording as for immediate
  recovery, so which read the single snapshot lands on cannot change the rule the message
  attributes.
- The point of the case is that no arrangement of reads makes the diagnostic blame the version
  rule.

### DriverAidlCompatibilityTest.AFailedObservationEmitsTheFallbackMessageAndPreservesTheCause

- `ThrowingMetadataDouble`'s metadata transaction fails outright.
- Evidence: the captured text is the fallback message rather than a specific one, and the
  caller's recorded cause is unchanged. A failed post-decision read must cost detail and must
  never overwrite the decision's own reason.
- The cause lives on the instance; the emitter is static, has no `this`, and cannot reach
  `availabilityReason` at all, so preservation is structural rather than a promise.
- The emitter must not propagate: a throwing diagnostic would reach `isServiceAvailable()`'s own
  handler, which records `REASON_QUERY_FAILED`, turning an established compatibility rejection
  into an unexplained query failure.

### Fake interface-metadata control cases (the four TheServiceFake…/TheControllerFake… cases)

- They sit in `DriverAidlCompatibilityTest` rather than their own fixture because a new fixture
  would sit outside `run_coverage.sh`'s classification constants, and a fixture in none of
  those lists breaks its selected-plus-excluded reconciliation. This fixture is already
  classified `CONTRACT_ANY_BACKEND_SUITES`, right for cases that need no registered service,
  no binder driver and no resolved back-end.
- Each fake's two metadata getters trace when, and only when, the reported value differs from
  the compiled-in constant. Three of those traces (the controller's version and hash getters,
  the service's version getter) had no caller that could fire them, because the setters that
  install a divergent value did not exist: the fake carried one hash setter, on the service.
  The controls exist because AAP §0.4.1 requires the fake to offer overridable
  `getInterfaceHash`/`getInterfaceVersion`, and these cases make each trace a reached branch
  rather than a merely reachable one. A control nothing exercises is indistinguishable from one
  that does not work.
- Per class, in order: the getter reports the constant and stays quiet before any override
  (the trace's false arm, which also stops ordinary runs being flooded); it then reports
  exactly what the setter installed and the trace names it (the true arm); and `reset()` puts
  both values back and silences both getters. The setters are traced and asserted too, because
  a silent change leaves a later divergence line with no visible cause.
- None registers: registration calls `defaultServiceManager()`, which opens the binder driver
  and aborts on this host, taking the whole binary down rather than failing.

### DriverAidlCompatibilityTest.TheServiceFakeReportsTheInterfaceMetadataInstalledOnIt

- Constructed locally and never registered, so nothing touches the service manager or the
  binder driver.
- Evidence: before any override both getters report the compiled-in constants and print no
  divergence line; after each setter the matching getter reports the installed value and the
  divergence line names it. The version half drives the service's previously unreachable
  version-divergence trace (no `setInterfaceVersion` existed to install a differing value).
- Wrong implementations caught: a setter storing nowhere the getter reads (the getter keeps
  reporting the constant), or a getter reporting a literal rather than the member (same
  symptom, with `reset()` silently a no-op).

### DriverAidlCompatibilityTest.TheServiceFakeRestoresBothMetadataValuesOnReset

- Evidence: both values are installed and observed changed, then after `reset()` both getters
  report the compiled-in constants again and print no divergence line.
- Matters beyond tidiness: the harness registers one fake for the whole process because the
  selection resolves once, so a surviving metadata value would decide every later case.
- Wrong implementation caught: a `reset()` that restores the hash and forgets the version,
  leaving the version-divergence trace firing for the rest of the run with nothing naming the
  cause.

### DriverAidlCompatibilityTest.TheControllerFakeReportsTheInterfaceMetadataInstalledOnIt

- The controller is constructed directly rather than obtained from a service: the controls are
  the controller's own and need no session. It is never registered; a controller is never
  published under a name, since a client obtains one from `open()`'s out-parameter.
- Evidence: both getters report the constants quietly before any override, then each reports
  the installed value and traces the divergence. These are the controller's previously
  unreachable traces (the class carried no metadata setter at all).
- Wrong implementation caught: a setter writing the service's member instead of the
  controller's, leaving both traces unreached while appearing to work.
- Not established: the middleware's compatibility check reads the service interface's metadata
  alone, so a divergent value here decides no selection outcome. The subject is the control
  and the fake's own reporting of it.

### DriverAidlCompatibilityTest.TheControllerFakeRestoresBothMetadataValuesOnReset

- Evidence: both values are installed and observed changed, then after `reset()` both getters
  report the constants again and print no divergence line.
- The service's `reset()` deliberately leaves the controller it owns alone, so a case
  configuring both must reset both; a reset missing either value would leak an override into
  the next case through the one long-lived controller the registered fake hands out.

### DriverAidlPreflightTest

- `DriverAidlImpl::isBinderPreflightOk()` is a private static taking the binder driver path and
  a context-manager timeout, both defaulted on its declaration to `DEFAULT_BINDER_DRIVER_PATH`
  and `DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS` (named by symbol, not line, because the file avoids
  citations into files it does not own). Taking the path as a parameter is what makes these
  cases possible: a nonexistent path and a non-binder node can be probed without rendering a
  runner's real `/dev/binder` unusable; nothing here writes to, chmods or unlinks the real
  node. The cases call the member through the befriended `BinderPreflightTestAccess`, which
  forwards their arguments unchanged; a file-local function in the `.cpp` would not be callable.
- Why the predicate must exist: on the pinned binder stack, reaching the service manager
  unguarded is unsafe in two independent ways. A missing or protocol-mismatched driver node is
  fatal rather than an error return, because the pin extends libbinder's failed-driver
  `LOG_ALWAYS_FATAL_IF` to plain Linux; and a working driver with no running servicemanager
  blocks indefinitely, because obtaining an `IServiceManager` polls until binder handle 0
  resolves with no bound of its own. Either defeats the absolute requirement that a supported
  SOC without an AIDL HAL reach the legacy back-end. On a host with no kernel binder support,
  the common case for this suite, the absent-path arm is what stands between `./run_L1Tests`
  and a SIGABRT taking every other case down.
- No driver precondition is established, as for the compatibility fixture: nothing touches the
  process-global driver, the HAL mock or the shared library.
- Self-sufficiency: each case supplies its own path; the one needing a real filesystem node
  creates, uses and unlinks it within the case body. No shared state is read or written.

### DriverAidlPreflightTest.DeclinesAnEmptyDriverPath

- Refused by the first decision point of `isBinderPreflightOk()`, before anything is opened.
  This is the arm a misconfigured deployment reaches (a path variable that resolved to
  nothing). Refusing explicitly stops `::open("")` being attempted and its errno reported as
  though a node had been found.
- Needs no back-end, no service and no filesystem node. Evidence: false for a
  default-constructed path.

### DriverAidlPreflightTest.DeclinesANonexistentDriverPath

- Declined because `::open` fails, at the second decision point. The arm the whole suite
  depends on: a host without kernel binder support has no `/dev/binder`, so this arm returns
  false, the selection falls back to legacy, and every other case runs. A regression would not
  fail one test; it would abort the process inside libbinder before the first test body.
- The chosen path is asserted absent first, so the case cannot drift onto another arm.

### DriverAidlPreflightTest.DeclinesAPathThatOpensButIsNotABinderDriver

- Declined at the character-device check (decision point 4 of 8): the regular file opens and
  its descriptor identifies, then its file type is refused, so the `BINDER_VERSION` ioctl is
  never attempted. "The path opened" and "the thing behind it is the binder driver" are two
  questions; a predicate asking only the first would pass a stale non-binder node straight to
  libbinder.
- Superseded: when written, the predicate had five decision points and a regular file reached
  the ioctl refusal, which was then the third. The ioctl-failure refusal, now decision point 6, is
  reached in this suite only through the synthetic probe, by the "decision point 6: ioctl fails"
  rows of `EveryPreflightArmReleasesExactlyTheDescriptorsItOpened` and
  `ClearsTheCustodySlotOnEveryDeclineSoNoStaleDescriptorIsReported`; the file-type refusal is
  also driven synthetically by `DeclinesANodeThatIsNotACharacterDevice`.
- A regular file is used because it is the one thing guaranteed to open `O_RDWR` on any host
  without being a character device; an anonymous `tmpfile()` cannot serve, because the
  predicate takes a path and opens it itself.
- The node owns its removal: it lives in the directory `TMPDIR` nominates when that validates,
  `/tmp` otherwise, and `TemporaryNode`'s destructor unlinks it, so fatal assertions can
  abandon the case without leaving a file, which a manual unlink after them cannot promise.
- Evidence: the node is asserted readable and writable first, so the verdict comes from a check
  after the open rather than the open. The case asserts only the false verdict; the preflight's
  `is not a character device` log line is what names the file-type check as the one refusing.

### DriverAidlPreflightTest.HonoursTheContextManagerTimeoutArgumentWithoutWaitingForAbsentNode

- The timeout is asserted at both ends of its range on a path that cannot get far enough to
  wait. Proves the two-argument overload is callable and neither value changes the verdict for
  an absent node, the property that matters in production: the default two-second bound must
  not turn a missing node into a two-second stall.
- Does not reach the context-manager arm, which needs the node to open and report a matching
  protocol first; a path and timeout cannot synthesise that without the real driver. That arm
  is reached through the synthetic probe by the case reporting the expected protocol and then
  declining the ping (see the file header's note on why the seam exists and what it does not
  change).

### DriverAidlPreflightTest.DefaultDriverPathIsTheNodeLibbinderOpens

- The default driver path is the one libbinder itself opens, and the default argument keeps
  that path out of every caller.
- Asserts the constant rather than the verdict: the verdict for the default path is a property
  of the host (false where there is no kernel binder support, true on a binder-capable
  runner), so asserting either would encode one host's configuration as a rule.
- Host-independent: the production call site names nothing
  (`DriverAidlImpl::isServiceAvailable()` calls `isBinderPreflightOk()` with no arguments) and
  neither does the harness's own preflight call (from `publishFakeForMode()`), so drift in the
  constants would silently change what both probe. The positive arm is reached by invocation
  B, where the AIDL back-end is selected only because the preflight passed.

### DriverAidlPreflightTest.DeclinesANodeWhoseProtocolVersionDiffersFromThisBuild

- Decision point 7 of 8 (the fourth of five when written): a node answering `BINDER_VERSION`
  with a version this build was not compiled against is declined, and the mismatch decides it.
  libbinder enforces protocol equality when opening the driver, in both directions, so a
  platform whose kernel speaks 7 against an SDK built for 8 fails every open. The preflight
  makes that degrade to "AIDL absent" rather than the pinned stack's abort.
- Reached with the substituted probe because no path can express it: a node either speaks
  binder (this build's protocol on a correctly provisioned host) or fails the ioctl one
  decision point earlier. The probe reports one greater than the compiled-in expectation, a
  mismatch on every host whether that expectation is 7, 8 or the 0 a build without the binder
  UAPI headers reports.
- The arm is identified, not merely the verdict: the ping is configured to succeed and
  `pingContextManager` is asserted never called. Otherwise a predicate ignoring the version
  and declining later would pass while letting a mismatched node reach libbinder.

### DriverAidlPreflightTest.DeclinesAMatchingProtocolWhoseContextManagerNeverAnswers

- Decision point 8 of 8, negative (the fifth of five when written). The second hazard, and the
  one that would hang rather than fail loudly: obtaining an `IServiceManager` polls until binder
  handle 0 resolves with no bound of its own, so a working driver with no servicemanager would
  stall `LibCCEC::init` forever rather than fall back. The bounded ping turns that into a
  verdict.
- The version matches exactly, so the ping is the only remaining decision, which makes the
  false verdict attributable.
- The deadline handed to the probe is asserted too: a predicate passing 0 or a constant of its
  own instead of the caller's timeout would leave the production two-second allowance
  unreachable. Evidence: one ping, on the opened descriptor, carrying the caller's 250 ms, and
  one close.

### DriverAidlPreflightTest.AcceptsANodeWhoseProtocolMatchesAndWhoseContextManagerAnswers

- The direct positive verdict. Every other preflight case is a rejection, and a predicate
  returning false unconditionally would pass them all; this case rules that out. It is the
  only assertion in the file that the preflight can say yes on a host without binder kernel
  support; the production positive arm is invocation B's.
- All eight decision points are passed, which the true verdict alone establishes: the predicate
  returns true only after the eighth. The steps the probe counts are asserted rather than
  assumed: path opened once, version read once, ping made once, descriptor released exactly
  once. The identity, file-type and owner checks pass on the probe's default well-formed node
  and are not counted here; their refusals have cases of their own. The open flags are checked
  because `O_RDWR` is a driver requirement: a read-only descriptor cannot carry a binder
  transaction.

### DriverAidlPreflightTest.ClampsAContextManagerTimeoutAboveTheCeilingToTheCeiling

- The ceiling, both arms (with the next case). The bound is an `unsigned int`, so a caller or a
  future configuration value can name a figure far beyond any plausible servicemanager
  start-up delay, every millisecond of it time `LibCCEC::init()` spends blocked before the
  legacy fallback is considered. The predicate clamps to `MAX_CONTEXT_MANAGER_TIMEOUT_MS`
  before probing.
- The probe records the bound it was handed, so the effective value is read rather than
  inferred from elapsed time, which on a loaded host would be a flaky measurement.
- Both arms are asserted because clamping unconditionally is as wrong as never clamping: a
  request below the ceiling must be honoured or the parameter is decorative. The ping declines
  so the case stays on the context-manager arm.

### DriverAidlPreflightTest.PassesAContextManagerTimeoutAtTheCeilingThroughUnchanged

- The boundary value is used rather than a comfortable middle one because `>` and `>=` differ
  only there: a clamp written with the wrong comparison would lower every request at the
  ceiling and nothing at any other value.

### DriverAidlPreflightTest.TheDefaultContextManagerTimeoutSitsBelowTheCeiling

- Asserted as a relation between two constants rather than as behaviour: if the default were
  raised above the ceiling, every initialization would silently take the clamp arm and the
  default would stop meaning what it says. This is the one place the relation is checked.

### DriverAidlPreflightTest.EveryPreflightArmReleasesExactlyTheDescriptorsItOpened

- Descriptor hygiene swept across the decision points in one table. The predicate opens the
  driver node itself and must release it on every exit. A leak is not cosmetic: the preflight
  runs at initialization in a long-lived middleware process, and on the pinned stack a
  lingering `/dev/binder` descriptor is a driver context nothing closes until the process
  exits.
- The six rows stop at decision points 1, 2, 6, 7 and 8 (both outcomes of 8), the five the
  predicate had when the table was written; the identity, file-type and owner refusals
  (decision points 3 to 5) each assert their one close in their own case.
- Rule: exactly one close per successful open, so the first two rows expect zero closes (an
  empty path opens nothing; a failed open has nothing to release, and closing a negative
  descriptor would be a bug of its own).
- One case rather than one per row because the property is uniform and a gap is easier to see
  in one table.

### DriverAidlPreflightTest.TheRestatedPosixNodeConstantsMatchTheSystemHeaders

- `ccec/src/DriverAidlImpl.hpp` restates `S_IFMT`, `S_IFCHR` and root's uid as its own
  constants because that header must compile where the binder kernel UAPI definitions are
  absent, so it includes no `<sys/stat.h>`. A drifted restatement would be invisible: the
  file-type test would compare against the wrong mask, every real character device would look
  like something else, and the AIDL path would be declined on every correctly provisioned
  platform with a message blaming the node.
- This is the one place the two are compared, and why the identity fixtures build modes from
  the POSIX macros rather than the production constants: the sides stay independent, so a
  wrong constant cannot agree with itself.
- The three permission constants are restated and checked the same way. They feed a diagnostic
  rather than a verdict, which does not exempt them: a wrong mask makes the permissive-node
  line report the wrong number, or nothing, on exactly the platform an integrator needs told.

### DriverAidlPreflightTest.DeclinesANodeWhoseDescriptorCannotBeIdentified

- Decision point 3 of 8: a node that opens but whose identity cannot be read is declined before
  the protocol ioctl. The identity is the only thing that gives the later re-check meaning
  (there is nothing else to compare the pre-lookup resolution against), so a preflight that
  shrugged at a failed `fstat` would hand libbinder a name certified on no evidence, and the
  pinned libbinder aborts when that name is not a usable binder driver.
- The ordering assertion is the second half: the protocol read must not have been attempted.
  An arm after the ioctl would spend a syscall on a node it was about to refuse and move the
  identity check further from the open it describes.

### DriverAidlPreflightTest.DeclinesANodeThatIsNotACharacterDevice

- Decision point 4 of 8. A regular file at `/dev/binder` is the shape a substitution takes when
  an unprivileged process wins a race to create the node, and also the shape of plain
  mis-provisioning. Refusing it here rather than leaving it to the ioctl arm matters because
  the identity carried to the pre-lookup re-check must belong to a node that is a binder
  driver, not merely something that answered `BINDER_VERSION`.

### DriverAidlPreflightTest.DeclinesACharacterDeviceThatIsNotOwnedByRoot

- Decision point 5 of 8. A character device at that path owned by anyone else is one an
  unprivileged process could create or replace, and selecting AIDL against it would put the
  whole HAL boundary (every frame transmitted and delivered) behind whatever published itself
  there. This arm is the half of that finding inside the middleware's reach; the other half is
  the platform prerequisite recorded on `isServiceAvailable()`.
- Only the owner changes, so a regression merging the type and owner checks fails here rather
  than passing by accident.

### DriverAidlPreflightTest.RetainsTheValidatedDescriptorAndItsIdentityOnlyWhenCustodyIsRequested

- Custody, both ways round, in one case because the halves are one property. The preflight
  validates a pathname and libbinder then opens the same pathname itself. Releasing the
  validated descriptor before returning left a window in which the node could change meaning,
  and on the pinned stack losing that window is a process abort. A caller intending to proceed
  to the lookup takes custody: the descriptor stays open, which also pins the inode so the
  identity cannot be satisfied by a recycled inode number, and the identity travels with it.
- A caller asking for nothing gets the original behaviour: descriptor released, process left
  as found. The harness's preflight assertion and every negative-arm case rely on that; a
  preflight retaining unconditionally would leak a driver context at each of them.

### DriverAidlPreflightTest.ClearsTheCustodySlotOnEveryDeclineSoNoStaleDescriptorIsReported

- The slot is cleared before any arm can return. A caller reads it after the call; a decline
  leaving it untouched would show whatever it held (in production a stack value; here a
  descriptor from a previous call), and the caller would release something it does not own or
  hold a custody window it never opened. Setting it to -1 first makes "not retained" and
  "stale" the same observation.
- `Decline` rows, in column order: row name, path, `openNode` answer, `identifyDescriptor`
  answer, reported mode, reported uid, protocol-read answer, reported protocol version, ping
  answer.

### Pre-lookup re-verification cases (TheServiceQueryDeclines…/TheServiceQueryAccepts…)

- The check and its re-verification are exercised as one call. They drive
  `isServiceAvailable()` rather than the preflight because the preflight's verdict is worth acting on only while it still describes
  the name libbinder is about to open, and the code establishing that sits between the
  preflight and the lookup, reachable only through the query.
- Each declines before the service lookup is entered, which makes them safe on a host with no
  binder transport: nothing reaches libbinder, and the synthetic probe guarantees it.
- The instance is local in every case, so the process-global driver and the resolved selection
  are untouched and the cases run identically under every invocation.

### DriverAidlPreflightTest.TheServiceQueryDeclinesWhenTheNameResolvesToADifferentNodeBeforeTheLookup

- The exploit closed: the preflight validates `/dev/binder`; before libbinder opens the same
  name, the node is replaced (unlinked and re-created, bind-mounted over, or a symlink
  re-pointed). Nothing in the middleware would notice, and libbinder's own open is a
  `LOG_ALWAYS_FATAL_IF` abort on the pinned stack, so the check guaranteeing a nonfatal legacy
  fallback would cause the abort it was written to prevent.
- The observation counters prove the decline happened at the identity comparison: exactly one
  path resolution, no second open, and the retained descriptor released.

### DriverAidlPreflightTest.TheServiceQueryDeclinesWhenThePathCannotBeResolvedBeforeTheLookup

- The node was removed between check and use. A failed re-resolution says nothing about
  whether the node is still the validated one, so treating it as "unchanged" equals not
  checking; it is refused, and the refusal is nonfatal, which is the point.

### DriverAidlPreflightTest.TheServiceQueryDeclinesAtThePreflightStageWhenTheDriverPathIsUnusable

- `isServiceAvailable()` declines in four ordered stages: preflight, custody re-verification,
  service lookup, compatibility. The neighbouring cases let the preflight pass and break the
  re-identification or liveness re-check, reaching the transport reason by the second
  assignment in the function. The two later stages need a binder driver and are unreachable
  from this host. This case drives the first stage, the preflight itself refusing, which every
  legacy-only SOC takes on every boot: the most-travelled path on real hardware, and the one
  whose failure would abort `LibCCEC::init()` instead of falling back.
- Why it needed writing: on a host with no binder driver the factory takes this arm unaided
  (the default path cannot be opened), so it is covered as a side effect of every driverless
  run. On a host with a usable driver, the only kind that can run the AIDL invocations, the
  factory never takes it, so the arm went unmeasured on exactly the runs that measure
  everything else. A test that only ran where the platform forced the arm was not testing it;
  this is why the branch-arm gate exists.
- The path is empty rather than a plausible absent filename: `isBinderPreflightOk()` rejects
  an empty path before calling the probe, so the probe-call assertions at zero (not "at most
  one") show the decline happened at stage one, ahead of any node work. The reason text is
  assigned at two stages and cannot distinguish them; a nonexistent filename would decline for
  the right reason at the wrong stage.
- A probe that would answer every question correctly is the control: a decline here cannot be
  attributed to a rigged probe, and the counters show it was not consulted.
- Complements the predicate's own empty-path case: that one shows the predicate refuses; this
  one shows `isServiceAvailable()` acts on the refusal (returns false, records the transport
  reason, issues no lookup).

### DriverAidlPreflightTest.TheServiceQueryDeclinesWhenTheReopenedDescriptorIsNotTheValidatedNode

- The pathname still resolves to the validated node but the descriptor reopened for the
  liveness check does not, so the reopen is validated in its own right rather than trusted.
  The liveness ping cannot use the retained descriptor (see the context-manager case), so a
  second one is opened, and a second open is a second resolution of the name with its own
  race. Comparing it against the same validated identity makes it inherit the guarantee
  instead of widening the window it was added to close.

### DriverAidlPreflightTest.TheServiceQueryDeclinesWhenTheReopenedDescriptorCannotBeIdentified

- A different arm from the case above, not implied by it: that one answers with a different
  node, this one with nothing. The re-verification's third point is a disjunction (refused if
  unidentifiable or identifying as another object), and a disjunction's two operands are two
  decisions. An `fstat` failing on a descriptor the open just returned is the shape of a
  revoked or torn-down binderfs mount; trusting the descriptor because the failure was in the
  check rather than the object would be the error the retained-identity path removes.
- The probe fails `identifyDescriptor` on its second call only: the preflight's own call is the
  first, and failing every call would decline at the preflight and never reach the
  re-verification.

### DriverAidlPreflightTest.TheServiceQueryDeclinesWhenTheContextManagerStopsAnsweringBeforeTheLookup

- What it bounds: `defaultServiceManager()` polls until binder handle 0 resolves with no upper
  bound of its own, so a wedged or dying servicemanager is the dominant stall mode on a
  healthy driver. Asking again under the preflight's bound turns a stall present at the re-ping
  into a decline. It bounds nothing after the re-ping, `getService` included: the pinned
  libbinder has no client-side transaction deadline, recorded as the residual acquisition window
  on `isServiceAvailable()`.
- The re-ping uses the second descriptor, and that is asserted, because the driver forces it:
  the ping maps the driver's transaction buffer, and the binder driver permits exactly one
  mapping per open descriptor for its lifetime (unmapping releases the range, not the right).
  A re-ping over the retained descriptor would fail on every correctly provisioned platform.
- Custody is held across the window, asserted through the release order. An open descriptor
  pins an inode; that is the only reason comparing `inode` across the window means anything,
  since without it the kernel may recycle the inode number for a replacement at the same path.
  `closeCalls == 2` counts releases but not when; `lastPingFd` proves the ping used the fresh
  descriptor but not that the retained one was alive. A "simplification" releasing the
  retained descriptor as soon as the preflight returned would keep both green and silently
  delete the pinning guarantee. The order distinguishes them: the fresh descriptor is released
  inside the re-verification and the retained one only when `isServiceAvailable()` returns, so
  the fresh one must be released first.
- This also pins the residual recorded on `BinderNodeIdentity`: the window is narrowed to its
  structural minimum, not closed, and the narrowing is worth exactly as much as the custody
  backing it.

### DriverAidlPreflightTest.TheServiceQueryDeclinesWhenTheValidatedNodeIsRePermissionedBeforeTheLookup

- The exploit: this is the half of the window a comparison of `(device, inode, rdev)` alone
  reported "unchanged". The preflight validates `/dev/binder` (character device, root owner,
  protocol matched, context manager answering) and carries its identity forward; before
  libbinder resolves the name, the node is `chmod`ed. Device, inode and rdev are untouched and
  the retained descriptor still pins that inode. An adversary unable to win a creation race can
  still widen write access, and the file-type and root-owner checks have already run, so only
  the re-verification enforcing its captured attributes stands in the window.
- The divergence is asserted through the log as well as the verdict, because a decline alone
  does not distinguish "the mode was compared" from "something else refused it": the numeric
  divergence mask names bit3, the mode.
- The premise (same object, different mode) is asserted: if fixtures drifted so another field
  differed, this would silently become a substituted-node case passing for the wrong reason.

### DriverAidlPreflightTest.TheServiceQueryDeclinesWhenTheValidatedNodeIsReOwnedBeforeTheLookup

- Separate from the mode case: `chmod` and `chown` are different operations and a comparison
  can cover one and miss the other. The consequence is worse here: the driver node is handed
  to an unprivileged owner who can then re-permission it at will. The preflight's root-owner
  check cannot catch it because it ran before the change.
- Everything but the owner is identical, so a decline can only come from the uid comparison,
  and bit4 of the divergence mask proves it.

### DriverAidlPreflightTest.TheServiceQueryAcceptsAnUnchangedNodeOnAllFiveIdentityAttributes

- The over-tightening guard. Adding mode and uid to the comparison can be overdone: comparing
  a value that legitimately differs between a `stat` of the path and an `fstat` of a
  descriptor would decline every correctly provisioned platform, and a suite of negative cases
  would not notice.
- It cannot be asserted by letting the query succeed: that enters `halcompat::getService`,
  which reaches `defaultServiceManager()` and aborts on a host without binder (SIGABRT, taking
  the binary with it). Instead both identity comparisons pass and the last of the four
  re-verification points, the second context-manager ping, is refused. Reaching it is the
  proof: it sits after the path comparison and the reopened-descriptor comparison, so
  `pingCalls == 2` happens only if all five attributes agreed, twice.

### DriverAidlPreflightTest.AcceptsAWorldWritableNodeAndReportsItsPermissionBits

- A regression guard whose direction is the point. Refusing a permissive binder node reads
  like a security improvement and is a production-breaking bug: a binder device node must be
  openable by every client using binder, so AOSP-derived platforms (this port vendors AOSP
  android-13.0.0_r74) publish it broadly accessible by design, and binderfs assigns its own
  mode. A restrictive-mode requirement would pass in this suite's CI guest (root-owned node,
  everything as root) and decline the AIDL path on every conformant production platform.
- The verdict asserted is true, and the line is asserted as an observation beside it: the
  middleware reports what it cannot require. What it does require (character device, root
  owner) is asserted by the cases above, and what it enforces across the window by the
  re-permissioned/re-owned cases.
- The line is logged at `LOG_INFO`, which the default level prints, so this case reads it
  without moving the level.

### DriverAidlPreflightTest.ThePermissiveNodeObservationLogsNoWarning

- Pins the level of the permissive-node line. Every standard binder node is `0666`, so a
  `LOG_WARN` line here fired on every healthy AIDL start and taught integrators to ignore WARN.
- The case sets the level to WARN with `ScopedCecLogLevel`, runs the same accepted preflight and
  asserts the line absent with the verdict still true. A non-root-owned node, refused at
  `LOG_WARN`, is run under the same level as the positive control: its line must print, so the
  absence is evidence about the line's level and not about a silenced capture.

### DriverAidlSelectionTest

- Requires invocation A: `CEC_TEST_AIDL_MODE=absent`, or unset, which means the same
  (established by `publishFakeForMode()`). SetUp asserts the precondition rather than adapting,
  so a mis-wired `--gtest_filter` fails with a diagnostic naming the mode instead of failing
  further in without explanation.
- No production introspection API exists and none was added, so the selection is observed two
  independent ways: a `dynamic_cast` against the two concrete types, which a test translation
  unit may name because neither `ccec/src/DriverImpl.hpp` nor `ccec/src/DriverAidlImpl.hpp` is
  an installed header; and the selected-path log line, the route the coverage runner and
  device-level validation use because the concrete headers are out of scope for them.
- Self-sufficiency: no case opens, closes or otherwise mutates the process-global driver; they
  only ask the factory which object it holds, a pure read. The one case that changes anything
  outside itself publishes a service, and its own notes record what that leaves behind.
- `SetUp()`: the selection resolves once per process inside `LibCCEC::init` and cannot be
  changed from the fixture, so the legacy resolution is asserted rather than arranged.

### DriverAidlSelectionTest.AbsentServiceSelectsTheLegacyBackEnd

- The first half of the "selection follows availability" success criterion (SC6(b) in the
  AAP): with no AIDL service registered the factory selects legacy.
- Asserted by concrete type in both directions (a `DriverImpl` and not a `DriverAidlImpl`): a
  one-sided assertion would also pass against a third implementation, and the two casts make
  "exactly one of the two" an assertion rather than a description.

### DriverAidlSelectionTest.SelectedPathLogLineNamesTheLegacyBackEnd

- The second half of SC6(b): the selected-path log line names the legacy back-end.
- The line the factory emitted went to the process log during `LibCCEC::init`, before any test
  body existed, so it cannot be captured; re-emitting it into the process log would break the
  runner's one-hit-per-process grep. The case drives the production logger with the
  transcribed contract format into an anonymous temporary and asserts the result.
- Evidence: the format's shape first (exactly one substitution, the `\r\n` terminator), then
  the captured line carrying the substituted name, then the two back-end names
  distinguishable. This proves what local string comparisons could not: the format is one
  `CCEC_LOG` accepts and substitutes into; the line survives the default log-level filter (a
  lowered default silently breaking every consumer is caught here); and the name agrees with
  the `dynamic_cast` result.

### DriverAidlSelectionTest.FactoryReturnsTheSameObjectOnEveryCall

- First half of SC6(c). `Driver::getInstance()` in `ccec/src/Driver.cpp` holds a reference to
  a function-local static (cited by name, not line, per the file's rule for citations into
  files it does not own). Three successive calls yielding one address make "resolved once"
  observable: a factory that re-decided per call, or returned a fresh object, would break the
  state machine every caller depends on, with Bus's reader and writer threads and LibCCEC each
  talking to a different driver.

### DriverAidlSelectionTest.RegisteringAServiceMidProcessDoesNotChangeTheResolvedBackEnd

- Second half of SC6(c), the one needing arrangement. The selection resolves once inside
  `LibCCEC::init` and is fixed for the process, so a late HAL must not be picked up half way
  through, leaving part of a session on legacy and part on AIDL.
- The direction is forced: removing a service is not expressible, since the pinned C++
  `IServiceManager` has no removal API and retains a reference to whatever was published (the
  file header's unreachable path (4)). Adding one is expressible and also the more dangerous
  direction in practice, being what a late-starting HAL produces.
- The preflight is not optional: `registerFakeHdmiCecService()` guards on the driver node and so
  cannot abort, but on a host with a driver node and no servicemanager `defaultServiceManager()`
  blocks unbounded until handle 0 resolves, hanging the binary. `isBinderPreflightOk()` covers
  both, so it is asked first, as the harness does in `publishFakeForMode()`.
- Two forms; the strong one is mandatory wherever possible. Strong (preflight passes): a service
  is published mid-process and the publication asserted, so "the resolved back-end did not
  change" has something to have changed to. Weak (preflight declines): nothing can be
  published, and the assertions reduce to the singleton identity the previous case covers.
  That is not SC6(c) evidence, and the case says so in its log line and every failure message,
  so a green result on a driverless host is not read as the strong form.
- The verdict decides the form, rather than the outcome being reported either way, because a
  case that only reported a refused publication would make the strong form unreachable: a
  runner where `addService` refused would print "was refused" and pass, silently losing the
  evidence on the hosts able to produce it. Inside the strong form a refusal is asserted, not
  reported: the preflight just confirmed a usable transport and reachable context manager.
- What it leaves behind: where registration succeeds the fake stays published for the rest of
  the process, as there is no withdrawal. No sibling suite is affected: the selection is
  resolved and cannot be re-decided, nothing else in the binary looks the name up, and the
  service manager's reference dies with the process. The strong reference lives in a
  function-local static rather than the fixture so the published binder outlives the case;
  dropping it would leave the service manager advertising a destroyed object, worse than
  advertising a live one.
- No gtest property records the form: the runner consumes this suite's JSON, and an unknown key
  changes a contract another file owns. The form is on stdout, kept in the runner's
  per-invocation log, and in every failure message.

## tests/L1Tests/ccec/test_DriverAidl.cpp (part 4 of 6)

Detail moved out of the comments of the `DriverAidlLocalInstanceTest` group: the fixture, the
receive-queue handoff helpers in its anonymous namespace, and its cases.

- **Superseded.** The original comments of this part described two pre-refine AIDL behaviours
  that no longer hold. `getPhysicalAddress()` was blocked on B1, logged the block and left its
  out-parameter untouched; it now reports the fixed physical address 1.0.0.0 with no AIDL call.
  `getLogicalAddress()` reported whatever the HAL held unprompted; the back-end now discovers one
  logical address from its own DeviceType when the driver is enabled and registers it with
  `addLogicalAddresses`, `getLogicalAddress()` still reads it through the HAL, and its `devType`
  argument remains log-only.

### DriverAidlLocalInstanceTest (group header)

- `DriverAidlImpl`'s constructor touches no binder: it sets the state to CLOSED, the legacy
  handle field to 0 and an empty address list, exactly as `DriverImpl`'s constructor does. That is
  what makes the factory's construct-then-query order safe on a legacy-only SOC, and what makes a
  local instance constructible where no service, service manager or binder driver exists.
- Every `status != OPENED` guard, the `writeAsync` prelude ordering, and the three methods that
  carry no guard at all are therefore reachable on a plain instance without a HAL of any kind.
- A local instance is used rather than the shared driver, following the idiom of the neighbouring
  async suite (`test_DriverImpl_Async.cpp`): the process-global driver is open and shared by every
  other suite in the binary, and closing it to reach a guard would hand a closed driver to
  whichever suite runs next.
- A plain local instance is never opened here: no case in the group calls its
  `isServiceAvailable()`, the only public call that caches a service proxy, so its `open()` raises
  `IOException` at the no-proxy guard, which a case in the group asserts. The real HAL's
  `IHdmiCec::open()` is single-instance and fails `EX_ILLEGAL_STATE` while a session is held,
  which is why the group does not open a second session under invocation B, where the
  process-global AIDL back-end holds one. The in-process fake does not enforce that rule, and
  `DriverAidlSessionTest` opens plain local instances against it
  (`AFailedCloseStillReleasesAReaderParkedOnTheIncomingQueue`,
  `ACallbackAfterAFailedCloseOrOwnerDestructionIsDroppedNotDelivered`).
- The group's probes do reach OPENED: `ReceiveQueueProbe::markOpened()` forces the state, and
  `SessionStateProbe::injectOpenSession()`, inherited by `AllocationProbe`, sets OPENED over
  injected in-process service and controller doubles (`IHdmiCecDefault` and
  `IHdmiCecControllerDefault` doubles, or locally constructed `FakeHdmiCecService` and
  `FakeHdmiCecController` objects and subclasses, none of them ever registered). Each probe's
  destructor sets CLOSED before `~DriverAidlImpl()` runs, which keeps the base destructor off its
  close path.
- Self-sufficiency: every case constructs its own instance in its own body and lets it go out of
  scope; nothing is shared between cases and nothing outside the fixture is written.

### Receive-queue ownership handoff helpers (anonymous namespace)

- The helpers and the five handoff cases at the end of the group exist for one production
  guarantee: `DriverAidlImpl::offerReceivedFrame()` must report whether the incoming queue actually
  took a frame, and `close()`'s NULL sentinel must be serialized against it on
  `queueProducerMutex` so the receive path's occupancy observation cannot go stale.
- Neither property is reachable through the public surface on a host with no binder driver,
  because OPENED only arrives through `open()`, which needs a live compatible AIDL service. Hence
  the test-local subclass, and hence `protected` on the back-end's internal section
  (`ccec/src/DriverAidlImpl.hpp`).
- Everything is in one anonymous namespace, so nothing it declares can collide with a name in
  another translation unit of the suite.

### ReceiveQueueProbe

- A subclass rather than the instance itself: `offerReceivedFrame()` accepts a frame only while
  OPENED, and OPENED needs a live service, so the ownership contract would otherwise have no
  coverage on this host. The subclass establishes the precondition and calls the handoff with no
  service, no listener and no threadpool, none of which the queue handoff touches.
- Forcing the state field is not the same as holding a session: `markOpened()` writes the
  middleware's own lifecycle field and nothing else. What the cases exercise is the queue
  arithmetic, the producer serialization and the ownership report; anything that would dereference
  a proxy is unreachable and not asserted.
- The destructor drains whatever the queue still holds and returns the state to CLOSED, so a case
  that ends early on a failed expectation neither leaks a frame nor leaves the base destructor to
  run `close()` into an `EventQueue` that would then report leftover elements.

### ReceiveQueueProbe::markClosing

- CLOSING is reachable only from the probe: production sets it inside `close()`, between the guard
  and the HAL call, so no case could otherwise observe `read()` in that state.

### ReceiveQueueProbe::postCloseSentinel

- The producer lock is taken because `close()` offers its sentinel under `queueProducerMutex` as a
  producer on the queue, not merely its terminator; a helper offering without it would model a
  close that does not exist.
- It places a sentinel while the instance stays OPENED (the close-first-by-occupancy ordering) or
  while a case holds the instance lock (the flush case), neither of which production `close()` can
  express.

### ReceiveQueueProbe::postFrameBehindSentinel

- The one producer path that skips the OPENED guard, modelling a real state: `offerReceivedFrame()`
  refuses once the instance leaves OPENED, so no frame can be added during a close, but a frame
  accepted while OPENED is still queued when `close()` runs, and `read()`'s flush meets it.
- `offer()` cannot express that: a frame offered before the close is consumed by the reader's own
  poll, so the flush never sees it, and a frame offered after the close is refused.
- With the instance lock held, the reader is parked between its poll and its state re-check.
- Taking `queueProducerMutex` makes the placed occupancy visible to the check in
  `offerReceivedFrame()`.

### ReceiveQueueProbe::instanceLock

- `read()` polls the queue and only then takes the instance lock to re-check the state, so a test
  holding it can queue a frame behind the sentinel while the reader is past its poll but cannot
  yet see the state change. Without the gate the reader consumes the frame in its poll-and-loop
  path and never reaches the flush.
- `close()` takes the instance lock first and the producer lock inside it, so a test may take
  `producerLock()` or call `postCloseSentinel()` or `postFrameBehindSentinel()` while holding this
  lock, which is what the frame-behind-sentinel flush case does. It must never take this lock
  while holding `producerLock()`, which would invert the only nesting order that exists.

### ReceiveQueueProbe::offer

- On a false return the caller still owns the frame.
- Nothing is reinterpreted, because the agreement between this value and what the queue actually
  holds is the property under test.

### ReceiveQueueProbe::producerLock

- It makes the close-side acquisition observable: the defect it guards against was a missing lock
  acquisition in `close()`, which a serial case cannot see. While a case holds this lock and drives
  the real `close()` from another thread, the sentinel offer must not complete; completing anyway
  is the regression.
- The lock itself is handed out, rather than a "take it for me" helper, so the test holds it across
  its own scope under `CCEC_OSAL::AutoLock`, as production does.
- Nothing the cases call while holding it needs the instance mutex: `occupancy()` and `take()`
  reach only the `EventQueue`'s internal lock.

### ReceiveQueueProbe::occupancy, take, drainCounting, drainInto

- An occupancy figure alone cannot tell a swallowed sentinel from one frame too many, which is why
  the counting drains exist alongside it.
- `take()` should be called only while `occupancy()` is non-zero; that is the Bus reader's
  contract too.
- `drainCounting()` turns "the queue holds N entries" into "the queue holds these frames and
  this many `close()` sentinels", the assertion that tells a dropped sentinel from a landed one.
  The caller must not hold a pointer to any released frame.
- `drainInto()` is the variant the ownership cases need: counting establishes how many frames the
  queue took, naming them establishes which ones, which is what an exactly-one-owner ledger is built
  from. A count cannot distinguish "the queue kept the frame the caller also released" from "the
  queue kept a different frame".
- Both drains read occupancy and poll strictly while it is non-zero, because `EventQueue::poll()`
  blocks on an empty queue.

### ReceiveQueueProbe::drain

- `EventQueue::poll()` leaves its `E front;` local unassigned on the arm where the queue empties
  between the size read and the poll, so every distinct call site produces its own pre-existing
  `-Wmaybe-uninitialized` diagnostic, and `osal/include/osal/EventQueue.hpp` is outside this
  migration. Funnelling through `take()` keeps that diagnostic count where it was.

### MonotonicLatch

- `CCEC_OSAL::ConditionVariable` is right in shape (a timed wait on a sticky condition, so a worker
  finishing before the test thread waits is still observed) and wrong in two details a bounded
  observation cannot tolerate:
  - its timed wait normalises only `tv_nsec > 1000000000`, not equality, so a wait begun exactly on
    a 600000-microsecond boundary passes `pthread_cond_timedwait` a `tv_nsec` of exactly
    1000000000; the call returns `EINVAL` immediately and the wait can spin instead of waiting;
  - it waits on `CLOCK_REALTIME`, so an NTP step or manual clock change moves the deadline under a
    wait in progress, and a 400 ms bound can expire instantly or last far longer.
- Both live in `osal/`, which the migration does not touch, so the bound is established here:
  `std::chrono::steady_clock` cannot be stepped, and the predicate form of `wait_until()`
  re-checks the deadline against that clock on every spurious wake.
- The OSAL types are still used where no timed wait is involved: the test thread holds
  `queueProducerMutex` under `CCEC_OSAL::AutoLock`, as production does.
- Stickiness is load bearing: a wait that starts after `notify()` still returns true.

### WORKER_ABANDON_DEADLINE_MS

- An unconditional `join()` on the disposition path would defeat every other bound in these cases.
  The disposition runs after the case body has finished, including after an early return from a
  failed assertion; by then the producer lock is released, so a worker can have left one
  `CCEC_OSAL::EventQueue::offer()` and, for `close()`, a transaction that fails immediately on an
  instance holding no proxy.
- What `offer()` does guarantee: it takes the queue's own mutex under `CCEC_OSAL::AutoLock`, so it
  can wait on whatever holds that mutex; it may allocate through `std::deque::push_back`; it
  signals its condition variable while holding the lock. It never waits for capacity (a full queue
  discards the element), so there is no unbounded wait inside it, but no constant-time or
  nonblocking property either.
- The deadline is therefore an empirical bound, not derived from that work: two seconds is orders
  of magnitude above what these workers measure, so a worker still running has stopped making
  progress. Waiting for ever would turn a reported failure into a hung suite, because the failed
  assertion would unwind into an unbounded join on the worker it just reported.
- `PRODUCER_COMPLETION_TIMEOUT_MS` is the bound a passing run must never hit and can be generous;
  this one is reached only after a failure has been reported, where a prompt report is worth more
  than another chance for a broken worker.

### BoundedWorker

- A bare `join()` at the end of a case is not enough: the concurrent cases park a worker on
  `queueProducerMutex` and use `ASSERT_*` while it is parked, and a joinable `std::thread`
  destroyed by that return calls `std::terminate`, replacing a reported failure with an abort that
  takes every remaining case with it.
- An unconditional joiner is equally bad: if a bounded completion assertion fails because a worker
  is stuck, unwinding runs the join and the run hangs on that thread.
- Disposal waits for the body for at most a caller-supplied bound of monotonic time
  (`WORKER_ABANDON_DEADLINE_MS` from `QueueHandoffOverlapHarness`, `kStalledTransmitBoundMs` from
  `StalledTransmitHarness`): if the body returned, `join()` waits out thread teardown only (the
  latch is the body's last statement); if not, `detach()` does not wait for the thread, and the
  state the abandoned thread can still reach is what the owning harness retains by design.
- A second `dispose()` of a detached worker returns true, because the thread is no longer
  joinable, so each harness records abandonment in a flag that is never cleared.
- Declaration order is part of the mechanism: the harness is declared in the case body outside the
  scope that holds the lock, and the lock is taken in an inner scope. Locals are destroyed in
  reverse order, so an early return releases the lock first, letting a parked worker finish, and
  only then disposes the workers. Disposing while the lock was held would make every disposal hit
  its deadline and abandon a worker that was merely parked.

### BoundedWorker::~BoundedWorker

- `QueueHandoffOverlapHarness` and `StalledTransmitHarness` dispose every worker before deleting
  any, so a joinable thread here would mean `dispose()` was never called. Detaching keeps even
  this path off an unbounded wait; each harness's retain-on-abandonment rule keeps the detached
  thread's state alive, and neither deletes a worker it abandoned.

### QueueHandoffOverlapState

- The counterpart of `BoundedWorker`'s abandonment route. Every object a worker can reach (the
  probe whose queue and locks it uses, the latches it signals, the flags it writes, the frame whose
  ownership it decides) lives here and is reached through a pointer, never as a reference to a
  case-body local; a stack local would be gone the moment the case returned, and an abandoned
  worker would write into a dead frame.
- One struct rather than one per case: the three cases need overlapping subsets of the same few
  fields, and a shared holder keeps the abandonment rule in one place.
- `allocated`: the ledger exists because a case returning early from a failed assertion would
  otherwise leak its frames, and because release is safe in only one order: after every worker has
  joined, and after the queue is drained, so the probe's own destructor cannot release the same
  frame a second time. The harness destructor is the one place that order is expressed.
- `producersAtRendezvous`: the second producer to arrive releases the pair, which makes the
  overlap real without the test thread being scheduled at the right moment and without leaning on
  the production lock, which is under test and absent in the mutation the stress case must catch.

### QueueHandoffOverlapHarness

- Disposal is bounded (`BoundedWorker::dispose`), so a stuck worker is detached, not waited on;
  a detached worker is still executing production code against this state, which is why the state
  and worker objects are leaked when any worker was abandoned.
- The leak is a few hundred bytes plus one queue, once, on a path that runs only after a case has
  failed. Freeing them would be a use-after-free in a thread nothing can synchronise with any
  longer, the one outcome worse than a leak.
- Destructor sweep order, taken only when every worker joined: nothing produces onto the queue;
  the queue is drained first, so the probe's destructor finds it empty and cannot release a frame
  the sweep is about to release; then every registered allocation is released exactly once,
  whatever the case asserted and however it returned, so an early-returning case reports its
  failure without leaking on top of it. If any worker was abandoned the sweep is skipped and
  everything is retained.

### QueueHandoffOverlapHarness::workerWasAbandoned

- A true result is also what makes the harness leak its shared state deliberately rather than
  free it.

### PRODUCER_LOCK_OBSERVATION_MS

- The bound on the negative observation in the two concurrent timing cases; the asymmetry it rests
  on makes them deterministic rather than racy.
- With the production lock in place the watched producer cannot complete whatever the scheduler
  does, so no window length produces a false failure; the window is pure cost in a passing run,
  hence a few hundred milliseconds rather than seconds.
- With the acquisition removed nothing holds the producer: the watched work (a size read, a deque
  push and a condition signal) waits on no lock the test holds, so the removal escapes only if the
  producer is kept from running for the whole window.
- Measured both ways before commit: with the acquisition deleted from `close()` both concurrent
  cases fail on this observation; with it restored both pass.

### PRODUCER_COMPLETION_TIMEOUT_MS

- The positive half of the same observation, reached only after the lock is released. The
  remaining work is one `CCEC_OSAL::EventQueue::offer()` (a queue-mutex acquisition, a possible
  `std::deque::push_back` allocation and a notification under that lock, with no wait for
  capacity) and, for `close()`, a transaction that fails immediately because a local instance
  holds no proxy. None of that is constant time, so the figure is an empirical margin, not derived.
- Ten seconds is a generous margin over that work, so a timeout means the producer stopped making
  progress rather than that it is slow. Bounded rather than an unqualified join, so a deadlocking
  regression is reported instead of hanging the run.

### OVERLAP_STRESS_ITERATIONS

- The illegal state the ownership contract forbids is reachable only while the two producers are
  genuinely interleaved: the sentinel must land inside `offerReceivedFrame()`'s narrow
  occupancy-check-to-offer window (a size read and the queue-lock acquisition of the offer).
- One overlap may miss it, so one iteration proves nothing; with a per-iteration hit rate measured
  here at roughly one in fifteen (20 repeats with the lock removed from `close()`, on a 4-CPU
  host), 256 attempts make a miss vanishingly unlikely. The case stays short: each iteration is
  two thread creations and about thirty offers, each a queue-mutex acquisition, a possible deque
  append and a notification.
- Detection does not rest on timing: each iteration asserts an invariant the fix makes
  unconditional and the defect makes violable; the iterations give the violation a chance to occur,
  not a wait a chance to expire.

### OVERLAP_RENDEZVOUS_BOUND_MS

- The bound on the only spin in the file. It is reached exactly when the partner worker never ran
  (a thread that could not be created); in a passing run the second arrival's store releases the
  spin. The spin is bounded on `std::chrono::steady_clock`, so no clock adjustment can extend it.

### OVERLAP_OFFSET_SPREAD

- Released from the same instant, the producers reach the queue with whatever fixed skew the
  scheduler and cache state impose; sampling only that alignment would sample one schedule 256
  times.
- The spread is a few hundred units, a unit being one volatile increment, sized to carry the sweep
  across the window the missing lock opens and past it on both sides.

### burnOffset

- It must neither sleep nor yield: both hand the CPU to the scheduler, which then decides when the
  thread resumes, so the offset would no longer be the loop's to set and a fine sweep would become a
  coin toss.
- A plain loop or a compiler barrier alone could be elided by the optimiser; a volatile store per
  unit cannot.

### awaitOverlapRendezvous

- A rendezvous the test thread had to release would need the test thread scheduled between the
  producers' arrivals, on a host where the producers are the runnable threads.
- A rendezvous built on the production producer lock (as the two timing cases use) releases
  nothing in the mutation this case must catch, because a `close()` that does not take the lock
  never parks on it.
- Handing the release to the second arriver removes both dependencies: one store by a producer
  releases the pair, with no third thread to be scheduled.

### ClosingServiceDouble

- `DriverAidlImpl::close()` has two failure shapes, a non-ok transaction and an ok transaction
  reporting false, and both raise `IOException` after the instance has released its session
  references. This double reaches both in direct, binder-free tests on a session injected into a
  `SessionStateProbe`; the session suite reaches them through the fake service's close controls,
  `setCloseResult()` and `setCloseBinderStatus()`.
- Not derived from `BnHdmiCec` for the reason `MetadataDouble`'s warning gives: a `Bn*` object
  could be registered and answer real lookups, which is unwanted from a case-local double.
- Every other method keeps `IHdmiCecDefault`'s `UNKNOWN_TRANSACTION`, so a case that accidentally
  reached one fails rather than silently succeeds.
- `open()` is not overridden because it is unreachable from a local instance on a host without a
  binder driver: `DriverAidlImpl::open()` calls `ProcessState::self()->startThreadPool()` before
  calling the service, and the pinned libbinder terminates the process when the driver cannot be
  opened. The reopen half of the B2 residual is asserted from the instance state and the status
  contract instead.

### AddressReportingDouble

- What an out-of-process HAL can actually send: the generated proxy reads `int32_t` entries
  straight out of the parcel, so the vector reaching `getLogicalAddress()` is an arbitrary
  sequence of 32-bit integers, not of plausible CEC addresses. This double feeds such values
  (256, -1) to direct, binder-free tests through `SessionStateProbe::injectServiceOnly()`; the
  fake service's `setLogicalAddressesResult()` can report them too.
- Not derived from `BnHdmiCec`, for the reason `MetadataDouble`'s warning gives. A case that
  reached any other method fails rather than passing quietly.

### TransmitResultDouble

- `sendMessage()`'s result is an `int32_t` the generated proxy reads out of a parcel and hands over
  as a `SendMessageStatus`, so a HAL can return a value outside the three documented enumerators.
  Neither the fake service nor a real HAL can be asked to, hence a raw integer cast on the way out
  rather than a stored enumerator.

### SessionStateProbe

- Injection rather than `open()`: the real `open()` would abort this process for the
  `ProcessState` reason recorded under `ClosingServiceDouble`. Injection suffices for the close-side
  claims because `close()` reads exactly three things (the state, the service proxy and the
  controller reference), each set to the value a successful `open()` would leave.
- `EventListener` is a protected nested class the header only forward-declares, so no test
  translation unit can construct one. These cases establish that `close()` releases the controller
  and the state on both arms, leaves the service proxy in place, and tolerates a null listener as
  its own guard promises; the session suite covers the listener detach against the fake service on
  a binder-capable host.
- Forcing CLOSED in the destructor keeps teardown from perturbing the call counters a case just
  asserted.
- `injectServiceOnly()`: `getLogicalAddress()` has no state guard, matching
  `DriverImpl::getLogicalAddress()`, so it is reachable on a closed instance; injecting session
  state as well would model a precondition the method does not have.
- `closedState()`: the lifecycle enum has no type name, so a case cannot spell
  `DriverAidlImpl::CLOSED` in an assertion, and a literal 0 would encode the value rather than the
  meaning.

### DriverAidlLocalInstanceTest (fixture class, SetUp, mock)

- Using a local instance rather than `Driver::getInstance()` is what makes the set
  invocation-independent: the same assertions hold whether the process selected the AIDL or the
  legacy back-end. A plain local instance here stays CLOSED because no case in the fixture calls
  its `isServiceAvailable()`, the only public call that caches a proxy; the fixture's probes force
  or inject OPENED, and their destructors return it to CLOSED.
- The sibling `DriverAidlSessionFixture` holds the properties that need an opened AIDL session and
  therefore have to be partitioned by invocation.
- The mock is held rather than used to set expectations.

### DriverAidlLocalInstanceTest.FrameLoggingPrecedesStateGuardInWriteAsync

- `writeAsync` takes the frame buffer and logs the frame before it locks and checks the state (the
  prelude at the head of `DriverAidlImpl::writeAsync()`, mirroring `DriverImpl::writeAsync()`'s
  prelude ahead of its guard), so a closed driver handed an empty frame reports the decode failure:
  not the invalid state, and not the operation-not-supported that is this back-end's authorized
  difference.
- Why `std::out_of_range` escapes: `printFrameDetails` constructs a `Header`, which reads
  `frame.at(0)` and raises `std::out_of_range` on an empty frame, and its handler catches only
  `Exception`. `Exception` and `std::out_of_range` are siblings under `std::exception`, not related
  by inheritance, so `catch (Exception &e)` cannot catch it. The neighbouring async suite records
  the same boundary for the legacy back-end.
- Were the prelude and the guard reordered, the first expectation would start reporting
  `InvalidStateException` and fail loudly. No service, proxy or process-global driver is involved.

### DriverAidlLocalInstanceTest.WriteAsyncPreludeOrderIsIdenticalOnTheLegacyBackEnd

- This is the half of the `writeAsync` difference that must be identical on both back-ends. The
  authorized difference is only what happens once the prelude and the guard have both passed
  (success on legacy, `OperationNotSupportedException` on AIDL); that arm needs an open driver and
  is asserted in the invocation-specific fixtures.
- Asserting here that the two failure modes reached before the guard passes are the same on both
  back-ends is what makes the difference a single declared one rather than a diffuse divergence.

### DriverAidlLocalInstanceTest.EveryStateGuardedOperationRefusesAClosedDriver

- The guards are swept together because the property is uniform (`status != OPENED` means
  `InvalidStateException` for all of them), and a gap is far easier to see in one list than across
  five cases.
- `poll()` is not redundant: it carries no guard of its own and reaches the guard through
  `DriverAidlImpl::write()`, so it is the one entry that proves the internal call goes to the
  right place.
- `read()` must refuse on entry: with an empty queue `EventQueue::poll()` blocks by contract, so a
  read past the guard would hang the whole binary rather than fail one case.

### DriverAidlLocalInstanceTest.AddressGuardsRejectEveryLogicalAddressWhileClosed

- The guard is a state check, not an address check, so no address may slip past it. A guard
  written as a validity check would pass the preceding case and fail here.

### DriverAidlLocalInstanceTest.CloseOnANeverOpenedDriverReturnsSilently

- Silence is the observable legacy behaviour, not a convenience: `DriverImpl::close()`'s throw is
  compiled out under `#if 0`, and the AIDL back-end carries the same `#if 0` block verbatim in
  `DriverAidlImpl::close()` so the two files read alike. A close that threw would break
  `LibCCEC::term` on any path that had not opened.
- Idempotence is what callers rely on: Bus reaches `Driver::close()` from two places and LibCCEC
  from a third, and none tracks whether another already did.
- Neither a legacy close nor an AIDL close can be attempted, since no session was ever held.

### DriverAidlLocalInstanceTest.AFailedCloseLeavesTheInstanceClosedWithNoSessionReferences

- Finding F4(d): the failing close is the one arm B2 makes reachable. `close()` maps the candidate
  `IHdmiCec::close()` onto the legacy `HdmiCecClose()`, and a failure reaches the caller as
  `IOException`; the instance must already be fully closed by then, because `LibCCEC::term()` and
  `~DriverAidlImpl()` both swallow that exception and a half-closed object would be
  indistinguishable from an open one.
- Both failure shapes are driven because they take different routes through the same condition:
  `!txn.isOk()` for a transport failure, and `!closed` for a transaction that arrived intact and
  reported refusal. A fix handling only one would pass a single-shape case.
- One claim per line of `close()`'s post-condition: the state is CLOSED and was set before the
  raise (visible after the catch); the controller reference is gone, so nothing holds a session the
  HAL may have dropped; the service proxy is retained, which makes a reopen attemptable; no listener
  reference survives (null to begin with, see `SessionStateProbe`); the local logical-address list
  is untouched, matching `DriverImpl::close()`; the HAL was called exactly once, and destruction
  does not call it again.
- The injection positive control: `close()` returns silently on any state other than OPENED, so a
  botched injection would produce a green case that never reached the HAL.

### DriverAidlLocalInstanceTest.AFailedCloseLeavesTheInstanceReopenableOrCleanlyFailing

- Finding F4(d), B2 residual: a failed close leaves this side clean but says nothing about the
  HAL's side. `IHdmiCec::close()` is the high-confidence candidate for `HdmiCecClose()` pending
  owner confirmation, and a close that reported failure may have left the session open on the far
  side.
  `IHdmiCec::open()` is single-instance and refuses a second session with `EX_ILLEGAL_STATE`, so the
  next `open()` on the instance has exactly two outcomes, both safe: it succeeds, or it raises
  `IOException`. The case pins both halves of that claim.
- Why the reopen is not driven end to end: `open()` calls `ProcessState::self()->startThreadPool()`
  before it reaches the service, and the pinned libbinder terminates a process whose binder driver
  cannot be opened (measured on the host: SIGABRT with "Binder driver '/dev/binder' could not be
  opened. Terminating"). The session suite, which needs a binder-capable host anyway, owns the live
  reopen. This case owns the two properties checkable without one:
  1. the instance is left in a state from which a reopen can be attempted: CLOSED (what `open()`'s
     own guard requires) with the service proxy still held (what it calls);
  2. `EX_ILLEGAL_STATE` is a non-ok status, the only thing `open()` tests before raising
     `IOException`, so the refusal cannot be mistaken for a successful reopen. Were it ever ok, the
     instance would go to OPENED holding a null controller.
- A second close is silent and reaches the HAL no further, so a caller retrying `term()` cannot
  churn a session it no longer holds.

### DriverAidlLocalInstanceTest.FreshInstanceHoldsNoLogicalAddressAndConsultsNoHal

- `isValidLogicalAddress` is a walk of the local list under the instance lock, byte-for-byte the
  legacy implementation, on either back-end. `Connection` is its only production caller, one
  address at a time.
- A single false answer would be consistent with an implementation that special-cased one
  address, hence the full sweep.

### DriverAidlLocalInstanceTest.OpenWithoutAServiceProxyRaisesIoExceptionAndTouchesNoLegacyHal

- The ordering is the substance, not the exception. The proxy is cached only by a successful
  `DriverAidlImpl::isServiceAvailable()`, so this case's instance, which never calls it, has none;
  the no-proxy guard at the head of `DriverAidlImpl::open()` checks for it ahead of
  `ProcessState::self()->startThreadPool()`.
- That order makes the case runnable on a host with no kernel binder support: reaching
  `ProcessState` first would raise SIGABRT and take the binary down rather than raise a catchable
  exception. It also keeps every plain local instance in the fixture CLOSED, since `open()` cannot
  reach OPENED without a proxy, and so keeps its destructor off the close path; probes that force
  or inject OPENED reset the state to CLOSED in their own destructors.
- An AIDL back-end that fell back to `HdmiCecOpen` when its own service was missing would be a
  second, undeclared selection point, so no legacy entry point may be reached.

### DriverAidlLocalInstanceTest.GetLogicalAddressReportsZeroWithoutAServiceAndIgnoresDevType

- Zero is the contract, not a shortfall. It covers four conditions (no proxy, a non-ok binder
  status, a successful call reporting no addresses, and the genuine address 0), distinguished in
  the log rather than in the return value.
- The legacy implementation zero-initialises its local and returns whatever the HAL leaves there,
  and `LibCCEC::getLogicalAddress` turns a zero into `InvalidStateException`, the existing signal
  callers handle for "no address". Any other sentinel would suppress that throw and hand a caller
  an address it does not hold.
- `devType` is swept because the argument is not honoured on either back-end. The AIDL back-end
  determines its address at `open()` from its own DeviceType and registers it with
  `addLogicalAddresses`; the `devType` argument stays log-only.

### DriverAidlLocalInstanceTest.GetPhysicalAddressIsBlockedOnB1AndLeavesTheOutParameterUntouched

- Superseded. The original comments described the pre-refine AIDL `getPhysicalAddress()`: blocked
  on B1, reporting the block at `LOG_EXP` with the marker "BLOCKED ITEM B1" and the awaited device
  settings HAL contract for the EDID byte read, and leaving the caller's out-parameter untouched,
  asserted through a seeded `0xDEADBEEF` sentinel. The case and its reasoning (why a sentinel, why
  the report is captured and matched, why no log-level guard is needed) are replaced by the
  assertion that every AIDL-path physical-address query returns 1.0.0.0 with no AIDL call.
- Still applicable: the legacy `HdmiCecGetPhysicalAddress` must not be substituted on this
  back-end. Besides being forbidden, it is unsafe: `HdmiCecOpen` performs logical-address
  discovery for source devices, which is CEC polling on the wire, and would put a second
  controller on the bus while the AIDL HAL is active.
- The throw verdict is taken by hand rather than with `EXPECT_NO_THROW` because stdout is
  redirected during the call: a failing macro inside that scope would write its diagnosis into the
  capture file instead of the run's log. Recording the outcome and asserting after the capture
  closes keeps every failure message on the real stdout.
- The report fragments were transcribed because the producer emits an inline literal rather than
  a named constant. Two distinctive fragments rather than one token or one whole line: a shorter
  match would be satisfied by unrelated text in the capture, and a whole-line match would fail on a
  reflow that changed nothing an operator relies on.

### DriverAidlLocalInstanceTest.GetPhysicalAddressAnswersWhileATransmitIsStalledInTheHal

- Lifetime model: the doubles, probe, frame, results and both workers live in a heap
  `StalledTransmitState` owned by `StalledTransmitHarness`; each worker body captures only that
  pointer and records any exception in the state instead of letting it leave the thread.
- `releaseAndDisposeWorkers()` releases the stall, then disposes each worker with
  `kStalledTransmitBoundMs`; it is idempotent and its abandonment flag is never cleared. The
  harness destructor runs it again and frees the state only when both workers joined; otherwise
  the state is retained, because a detached worker may still hold the probe's instance lock inside
  `write()`, and `~DriverAidlImpl()` takes that lock.

### DriverAidlLocalInstanceTest.TheModelledSinkCallPathsStillMatchTheRealSinkSource

- The one case in the file that asserts about a file rather than a driver.
- `DriverAidlSessionTest.AddLogicalAddressFailuresReachBothRealSinkCallPathsAsExpected` measures
  authorized difference 3 through the two Sink call paths by modelling their exception handling: a
  try with an `IOException` arm and a generic arm for the first, an uncaught propagation for the
  second. The model is evidence only while it describes the plugin; by itself it would survive the
  plugin being reformatted, a catch arm being removed, the enable-time call acquiring a try block,
  or the Sink ceasing to call the middleware, while the claim about the caller became false.
- So the structure is read from the plugin source and checked against the model: exactly two calls
  to `LibCCEC::getInstance().addLogicalAddress(...)`; the first inside a try whose handlers include
  an `IOException` arm and a generic `catch(...)` arm; the enable-time one inside no try block.
  Those are the three structural facts the modelled case depends on; nothing is asserted about what
  the handlers do, which is the plugin's business.
- On path 1 the `IOException` arm is where a transport failure lands and the generic arm is where
  a refusal falls through; path 2 sits outside every try, which makes the modelled
  uncaught-propagation assertion the right shape for it.
- What it does not do: it does not compile or link the plugin (its own test binaries link the test
  framework's CEC mock in place of this middleware, and building it here would pull in Thunder), so
  it proves the call shape has not drifted, not that the plugin behaves as modelled at run time;
  that half remains the plugin's own suite and the middleware-level L3 step. Nor does it read the
  plugin's line numbers: the modelled case cites them for a human reader, and a citation drifting is
  a documentation matter, whereas structural drift would silently invalidate an assertion.
- Why this fixture: it runs on every invocation, including the legacy-only ones. A host without a
  binder driver never runs the session suite, and a drift guard that ran only where binder exists
  would be absent from the cheapest runs.
- The source is a required acceptance input, not an optional diagnostic: a case that printed a
  paragraph and returned would be recorded as a pass, so an acceptance run could go green while the
  only check tying the model to the real plugin never executed.
- Location outcomes:
  - `CEC_SINK_CALLER_SOURCE` set and readable: that file is used, and its path is reported so the
    log records which revision was measured;
  - set and not readable: hard failure, a wiring fault (the environment claims to provide the
    source and has not);
  - not found by the variable or any candidate path: hard failure, a missing required input; every
    path tried is named, with the two ways to supply it.
  The two failures keep distinct wording on purpose: "you told me where it is and it is not there"
  and "nobody told me and it is not beside me either" call for different fixes.
- Provenance: both CI workflows check the Sink component out at the reviewed immutable commit
  `2ad7e1a4712908a4d0fb838caebcbfcaabba55d8` and export `CEC_SINK_CALLER_SOURCE` to the absolute
  path inside that checkout, as a required step, so a network or permission fault fails the job
  instead of disabling the guard. The guard reads a reviewed revision, and the asserted structural
  facts were read from it; a drift is noticed when somebody deliberately bumps the pin and a human
  is present to decide whether the model must be re-derived.

### DriverAidlLocalInstanceTest.TheLogLevelGuardRaisesTheLevelAndRestoresItVerifiably

- `ScopedCecLogLevel` mutates two pieces of process-global, host-global state: the file production
  hardcodes at `/tmp/cec_log_enabled` (in a world-writable directory shared with every process on
  the host) and the file-static `cec_log_level` it feeds. Its only other caller is a
  `DriverAidlSessionTest` case, which requires the AIDL back-end, so on a host with no binder
  driver the custody protocol would be compiled and never executed. This fixture runs under every
  invocation and needs no service or back-end.
- What is asserted, and why none is implied by the others:
  - the guard raises: the effective level after construction is DEBUG, observed by emitting at
    each level rather than read from the file, since the file is production's input, not the state
    under test;
  - the raise is a move: the entry level is recorded and the case asserts return to it rather than
    any particular value, so a host legitimately running at DEBUG still passes;
  - restoration is verified: `restoreAndVerify()` must report success, and it checks publication,
    then the file's bytes or absence, then the effective level, while still holding the custody
    lock;
  - the level is back afterwards, observed independently, so a `restoreAndVerify()` returning true
    without doing anything still fails;
  - the second call is idempotent, because the destructor makes one and a double restore that
    rewrote the host's file would be a defect;
  - nothing is left at the shared path: it is absent or a regular file, and never the guard's
    temporary.
- A refusal is a failure, deliberately. If another `run_L1Tests` holds the custody lock, or the
  path is a symlink or owned by another user, the guard refuses and reports why, and the case fails
  with that reason rather than passing because the guard was careful.

## tests/L1Tests/ccec/test_DriverAidl.cpp (part 5 of 6)

### DriverAidlLocalInstanceTest.ReceiveQueueHoldsTheLegacyThirtyTwoEntriesAndDropsTheCloseSentinelWhenFull

- Pins the legacy queue contract: received frames and `close()`'s NULL sentinel share the 32
  slots `DriverImpl`'s queue gets, so all 32 frames are accepted, the 33rd is refused and stays
  the caller's, and a `close()` against the full queue has its sentinel dropped by
  `EventQueue::offer()`, as on the legacy path.
- Order of assertions: fill to `capacity` through the production handoff, every offer accepted,
  the one taking the 32nd slot included; the next frame is refused and the caller releases it;
  `close()` leaves the occupancy at capacity; a post-close frame is rejected; the drain counts
  exactly the frames the handoff claimed and no sentinel.
- A queue larger than the legacy one fails here twice: the occupancy after `close()` exceeds 32
  and the drain counts a sentinel. A handoff that refused early fails on the fill.
- Ownership follows the handoff's report throughout, so broken code produces reported failures
  rather than a double free that aborts before the later assertions. A frame is deleted only where
  the handoff said the caller owns it; deleting unconditionally would double-free against code
  that reported acceptance.
- The serialization of the two producers (`close()` taking `queueProducerMutex`) is invisible to
  this case and the next: both are serial, so deleting `{AutoLock lock_(queueProducerMutex);` from
  `close()` leaves them passing. It is measured by
  `CloseSentinelOfferBlocksOnTheProducerLockAConcurrentTestHolds`,
  `CloseSentinelOverlappingAReceiveHandoffLeavesEveryFrameWithExactlyOneOwner` and
  `ManyRealOverlapsKeepTheHandoffReportAndTheQueueInAgreement`; the five cases together cover both
  halves and no subset is sufficient.
- Legacy capacity parity: the capacity is asserted against the literal 32, the default
  `EventQueue(size_t cap = 32)` that `DriverImpl`'s queue gets. Every other figure in the case
  derives from `capacity`, so without the literal it would pass against a queue of any size. The
  same equality is a `static_assert` in `ccec/src/DriverAidlImpl.hpp`; the static_assert pins the
  constant and the runtime assertions prove the code path honours it (32 frames accepted, the
  33rd refused).
- `close()`'s IOException is expected: a local instance holds no service proxy, so the close
  transaction reports DEAD_OBJECT. The sentinel is offered before that transaction.
- The post-close rejection is `getIncomingQueue()`'s state guard raising before anything is
  offered; that exception drives the caller's release, the same mechanism as the legacy path.

### DriverAidlLocalInstanceTest.EveryFrameOfferedToAFullReceiveQueueHasExactlyOneOwner

- A ledger rather than an allocation count: the defect is not that a full queue drops something
  (it must) but that the handoff's return value stopped agreeing with what the queue did, leaving
  no owner. Each allocation is recorded with the side production said owns it, and the drain must
  partition the allocations: a frame in neither set was accepted and dropped (the production
  leak); a frame in both would be released twice.
- Not reproduced here: the "owners == 0" arm needs `close()`'s sentinel to land unserialized in
  the check-to-offer window, a genuine interleaving no single thread can stage.
  `CloseSentinelOverlappingAReceiveHandoffLeavesEveryFrameWithExactlyOneOwner` stages it by parking
  both producers on the lock. This case establishes the accounting (exactly `capacity` received
  frames accepted, every later one refused), with the partition asserted alongside so a change
  that fixed the count and broke the report cannot pass.
- Membership is a nested scan rather than a set: the ledger is a few tens of entries with a fixed
  bound, and a set would add an include to a translation unit whose include block is documented
  entry by entry.
- Offers run well past the limit because a stalled Bus reader keeps the queue full for as long as
  the HAL keeps delivering, and every further event leaked a frame.
- The drain takes the entries out itself rather than leaving them to the probe's destructor,
  because each entry's identity is the evidence; the final release happens only after the
  partition has shown no entry is owned twice or orphaned.

### DriverAidlLocalInstanceTest.CloseSentinelOfferBlocksOnTheProducerLockAConcurrentTestHolds

- Exists because the two serial cases cannot see the lock: deleting
  `{AutoLock lock_(queueProducerMutex);` from `close()` leaves both passing.
- Method: the test thread takes `queueProducerMutex` through the probe's `producerLock()`
  accessor and drives the real production `close()` from a second thread. `close()` takes the
  instance lock, moves the state to CLOSING and then blocks on the producer lock before its offer.
  Observation 1: the worker does not complete within `PRODUCER_LOCK_OBSERVATION_MS` (impossible
  with the acquisition; without it nothing holds `close()` back). Observation 2: the
  occupancy does not move while the lock is held (the sentinel has not landed), the same fact read
  from the queue, so a regression is reported twice.
- After the release the worker must finish, which proves "blocked" rather than "never started" or
  "deadlocked"; a case asserting only non-completion would pass against a `close()` that hung.
- It calls production code, not a copy: restating
  `{AutoLock lock_(queueProducerMutex); rQueue.offer(0); }` would keep passing after production
  stopped taking the lock.
- The failure direction is deterministic: no schedule lets `close()` complete while another thread
  holds the lock it must take, so a passing run cannot flake, and the work left once unblocked is
  one deque push, so 400 ms of margin cannot hide a missing acquisition. Checked
  both ways: with the acquisition deleted the case fails on observation 1.
- No sleeps: every wait is a bounded `MonotonicLatch` wait on `std::chrono::steady_clock` that
  returns the moment the worker signals and that a wall-clock step cannot move; the only elapsed
  time in a passing run is the deliberate negative observation.
- Limitation: `closeEntered` is signalled before `close()` is called, so it proves the worker
  started, not that it reached the acquisition. A worker descheduled right after signalling
  satisfies the non-completion observation for a reason unrelated to the lock, and every later
  assertion still holds once the lock is released; that counterexample cannot be closed from this
  side of the latch. The case is kept because it detects a missing acquisition immediately, as one
  specific readable observation.
- The probe, latches and worker flags live in `QueueHandoffOverlapHarness`, captured as a pointer
  by value, because an abandoned worker outlives the case body. One resident frame keeps the queue
  non-empty so an early sentinel visibly changes the occupancy, and that frame's ownership is
  settled before any assertion that can return. The worker pointer is declared outside the lock
  scope because the completion observation after it needs the same worker.

### DriverAidlLocalInstanceTest.CloseSentinelOverlappingAReceiveHandoffLeavesEveryFrameWithExactlyOneOwner

- A real interleaving is attempted: each producer (one entering `offerReceivedFrame()`, one
  entering `close()`) signals entry and is observed not to complete while the test holds
  `queueProducerMutex`. The latch is set before the production call, so the observation does not
  prove the worker reached the lock. Which wins once the lock is released is the scheduler's
  choice; both orders are legitimate and the assertions are written against whichever happened.
- The occupancy is `capacity - 1` when the race starts, so the two producers contend for the last
  free slot and the orders are observably different. Receive first: the handoff sees one free
  slot, accepts, and fills the queue; the sentinel then meets a full queue and is dropped, as on
  the legacy path. Close first: the sentinel takes the last slot; the handoff then refuses and the
  caller keeps the frame.
- The invariant in both orders: exactly one producer lands, so the sentinel is queued exactly when
  the frame was refused, and the handoff's return value agrees with what the queue holds. Without
  both acquisitions the sentinel can land inside the handoff's check-to-offer window, the frame is
  dropped and true is returned anyway.
- The close worker is started only after the receive side is observed not completing, which makes
  it likely, not certain, that the handoff passed `getIncomingQueue()`'s OPENED guard first; a
  state-guard refusal is reported by the post-join assertion rather than ruled out.
- The ledger: every frame is owned by the queue (drained by identity) or by the caller (the
  handoff returned false); both would be a double free, neither is the ownership leak.
- The receive worker's InvalidStateException arm is reachable if the driver left OPENED before
  that thread read the state; the post-join assertion reports it rather than tolerating it. An
  unexpected close exception surfaces through the sentinel-count assertion. Frames are released
  exactly once by the harness ledger after every worker joined and the queue is drained, on any
  exit including an early return; if a worker was abandoned they are retained instead.

### DriverAidlLocalInstanceTest.ManyRealOverlapsKeepTheHandoffReportAndTheQueueInAgreement

- A difference in kind from the two timing cases. They require non-completion while they hold the
  lock, but each signals its "entered" latch before calling production, so a worker descheduled
  after signalling satisfies the observation for an unrelated reason: unlikely, not impossible.
  This case proves the property rather than the timing.
- The invariant: reported-accepted if and only if the frame is in the queue. Its violations are
  acceptance reported for a frame `EventQueue::offer()` discarded (no owner) and refusal reported
  for a frame the queue took (the caller frees a frame the queue still holds). Also asserted: the
  sentinel is queued exactly when the frame was not, because one slot was free, and drained frames
  plus handed-back frames partition the allocations.
- Staging: each iteration fills to `capacity - 1` through the production handoff, so both orders
  are expressible and neither is a no-op, then releases two workers from this file's own
  rendezvous (`awaitOverlapRendezvous`: the second producer to arrive releases the pair). Not the
  production lock, which releases nothing when `close()` stops taking it (the very mutation to
  catch), and not the test thread, which would have to be scheduled between two arrivals on a
  host whose runnable threads are the producers.
- The alignment is swept: `burnOffset()` is applied after the release, alternating which producer
  carries it and varying its size, so the 256 attempts walk the relative arrival across the window
  instead of retrying one schedule.
- History of the census: an earlier revision asserted that both outcome orders appeared at least
  once in 256 attempts. With two CPUs every run produces tens of each; pinned to one CPU
  (`taskset -c 0`) the sweep produced 256 close-first-refused-by-the-state-guard outcomes (the
  close worker runs to completion before the receive worker is scheduled) and a red suite with no
  product regression. Single-CPU runners, busy shared CI executors and one-CPU container quotas are
  valid environments, so a census assertion made the verdict a property of the machine. That is
  why the orderings are constructed.
- Part 1 builds each ordering single-threaded through the seams `ReceiveQueueProbe` exposes
  (`postCloseSentinel()` for a sentinel queued while the instance is still OPENED, the real
  `close()` for one that also leaves OPENED) and asserts the same three invariants on each. It runs
  on every scheduler and any CPU count, and its arms, not the census, prove the accepted-and-present
  and refused-and-absent arms were exercised. Part 2 keeps the contended sweep, because a real
  overlap is the only thing in the file that puts both production producers on one queue at the
  same instant; its census is printed, not asserted.
- Do not simplify Part 1 away: the sweep does not reach the three outcomes reliably (one CPU yields
  one outcome 256 times), and deleting Part 1 while restoring a census assertion restores the false
  failure.
- Without `{AutoLock lock_(queueProducerMutex);` in `DriverAidlImpl::close()`, an overlap whose
  sentinel lands inside the handoff's check-to-offer window fails on the invariants (acceptance
  reported for a frame the drained queue does not hold, so it has no owner, and a sentinel queued
  beside an accepted frame), not on a timeout.
- Allocation accounting: each frame is registered in the harness ledger as it is allocated and
  released once after the workers are disposed and the queue drained; nothing here deletes a frame
  itself and nothing leaks if an assertion stops the loop early.
- Part 1 details. `ConstructedOrdering` names the one varying step (where the close side runs
  relative to the handoff), so the arms are one shape rather than three near-copies.
  `driveProductionClose` writes the same three outcome flags as the sweep's close worker, so both
  parts read the close side through one vocabulary. `constructOrdering` uses EXPECT throughout,
  because a fatal assertion inside a lambda returns from the lambda alone and would read as a
  silently skipped arm; it returns whether the arm produced its ordering, for the final
  non-vacuity assertion. The occupancy arm leaves the instance OPENED because the guard runs ahead
  of the occupancy check: moving the state would make it a second copy of the state-guard arm. It
  models the window between `close()`'s sentinel offer and its own state store. The
  `capacity - 1` fill also drives the room-available side of the full-queue check
  `capacity - 1` times on every arm. Each arm's decisive outcome is asserted, which makes the arm
  the ordering it claims rather than whichever ordering the code took.
- Part 2 details. A fresh instance per iteration, so no iteration inherits another's queue, state
  or rendezvous. Bounded completion and then a bounded disposition: the first says the producers
  finished, the second that they were joined rather than abandoned, and only a join is the
  happens-before for reading the plain flags. An abandoned worker's state is leaked by design and
  the loop stops instead of reading it. The first violated invariant stops the loop (one clear
  failure rather than 256). Invariant 1's reason: with one slot free, a sentinel beside an accepted
  frame means the two producers were not serialized. Invariant 2 is the one the missing lock
  violates, in either direction. A 0 / 0 / 256 census is what one CPU produces and is a fact about the machine,
  not a defect. The census assertions were removed because they made the verdict depend on the
  runner's CPU count; the constructed-arm assertions fail if an arm degenerates into a copy of
  another.

### DriverAidlLocalInstanceTest.TheReceiveFlushReleasesARealFrameQueuedBehindTheCloseSentinel

- No other case drains a frame through `read()`'s flush loop: every other receive case is consumed
  by the reader's checked poll, a different line.
- Reaching the flush deterministically: `read()` flushes only when its poll yields NULL and the
  state is no longer OPENED when it takes the instance lock; otherwise it loops. The gate
  sequence: (1) OPENED with an empty queue, so the reader blocks in `poll()`; (2) the test thread
  takes the instance lock, moves the instance to CLOSING, offers the sentinel and queues a frame
  behind it, so the reader consumes the sentinel and blocks on the lock; (3) the lock is released
  and the reader sees CLOSING and flushes the frame, then raises.
- The frame is placed behind the sentinel while the gate is held, the only arrangement in which the
  flush meets it: offered before the close the poll takes it, offered after it
  `offerReceivedFrame()` refuses it. Behind the sentinel it models a frame the HAL delivered a
  moment before the close.
- Nothing on this path reaches the legacy HAL: the receive queue and its sentinel are
  middleware-side on both back-ends.
- The reader-start margin is generous; a margin too short does not give a wrong answer but a
  reported missed flush. An occupancy of one means a harness sequencing failure rather than a
  production one, and is reported as such.
- Superseded: a companion case, `TheReceiveFlushSurvivesASecondCloseSentinel`, drove a second
  sentinel into the flush to prove a null check that read() no longer carries. With the legacy
  flush loop restored that case is a null dereference that crashes the runner, so it was removed;
  the defect it exercised is the shared legacy one recorded under read().

### DriverAidlLocalInstanceTest.TheReceiveGuardRejectsACallbackDuringAndAfterClose

- A binder threadpool thread delivers frames while another thread may be inside `open()` or
  `close()` writing the state under the instance mutex. The guard in `getIncomingQueue()` reads the
  plain-`int` state without that mutex, deliberately, reproducing the legacy accessor's unlocked
  read and its data race.
- Superseded: this entry formerly said the member was a `std::atomic<int>`, making the read a
  defined atomic load; the member is now a plain `int`, as `DriverImpl::status` is.
- Established deterministically: the guard's verdict in each state (accepted while OPENED, rejected
  while CLOSING and CLOSED), and a rejection leaves the frame with the caller so the listener's
  catch can release it. These verdicts are what an unlocked read of a torn or cached value could
  change.
- CLOSING matters most and is unreachable any other way: production sets it inside `close()`,
  between the guard and the HAL transaction, so a callback arriving during a close sees exactly
  this. Pinning the verdict per state keeps a guard regression from hiding behind a scheduler that
  happened not to interleave.
- Destruction while a callback is in flight is covered by
  `DriverAidlSessionTest.ACallbackAfterAFailedCloseOrOwnerDestructionIsDroppedNotDelivered`, which
  needs a live listener and therefore a binder-capable host.

### DriverAidlLocalInstanceTest.TheLogicalAddressReadAcceptsOnlyContractRangeValues

- `getLogicalAddresses()` delivers an arbitrary 32-bit integer read out of a parcel by the generated
  proxy. The AIDL contract says a logical address is 0x0..0xE, and nothing between an out-of-process
  HAL and this back-end enforces it.
- Validating after the conversion would not work: `LogicalAddress` carries the value through a
  narrower type, so 256 truncates to 0 and 271 to 0xF, both plausible to a later check. An accepted
  value would reach `Connection::matchSource()`, which rewrites the initiator nibble of outbound
  frames, putting an address the HAL never reported on the CEC wire.
- The rejection path is the method's existing no-address return, 0, which
  `LibCCEC::getLogicalAddress()` turns into InvalidStateException. The genuine address 0 is in the
  table because it is indistinguishable from "no address" by design on both back-ends.

### DriverAidlLocalInstanceTest.TheLogicalAddressReadValidatesEntryZeroOfAMultiAddressResult

- Entry zero is the only entry the back-end uses: a multi-address result is logged and its first
  entry used, with no iteration and no multi-address state. An out-of-contract entry zero is
  therefore rejected even when a later entry is valid; scanning for the first acceptable entry would
  be exactly that forbidden iteration.
- In the rejected row, 271 would have truncated to 0xF had it been converted first.
- The non-ok transaction arm predates this validation and is asserted again so the validation
  cannot have displaced it.

### DriverAidlLocalInstanceTest.EveryDocumentedTransmitStatusKeepsItsLegacyMapping

- Asserted again because the status translation is now a switch: with the broadcast sense inverted,
  a refactor of the if/else-if chain into a switch has to be shown not to have swapped an arm. Each
  row states the destination kind, the reported status and the outcome the legacy back-end gives.
- The transmit is attempted exactly once in every arm, including the raising ones, because the
  status is translated after the call and never instead of it. The probe's destructor forces the
  state to CLOSED so no teardown re-enters `close()` against the doubles.

### DriverAidlLocalInstanceTest.AnUndocumentedTransmitStatusReturnsNormallyAsTheLegacyMappingDoes

- `sendMessage()`'s result is an int32 the generated proxy reads out of a parcel, so a buggy, newer
  or hostile HAL can report a value that is none of the three enumerators. The legacy mapping takes
  no action on a status outside its failure set and its not-acknowledged arms, so such a value
  reaches "Send Completed" there, and this back-end must return normally for it too.
- Both destination kinds are driven because a default arm placed inside one destination's branch
  would cover only half the cases, and the broadcast frame carries `REPORT_PHYSICAL_ADDRESS` so no
  value may stray into the CTS 9-3-3 arm. 3 is the most likely accident; the negative and maximal
  values model a misinterpreted status code or a hostile fake. Each write still sends exactly once.
- *Superseded:* this case was previously named
  `AnUndocumentedTransmitStatusIsTreatedAsAFailedTransmit` and required `IOException` for every
  value; review removed that raise as an observable difference outside the authorized list.

### DriverAidlLegacyArmTest

- Requires invocation A (`CEC_TEST_AIDL_MODE` absent or unset), because the cases drive the
  process-global driver and it must be the legacy one. SetUp asserts this with a message naming the
  mode, so a mis-wired filter fails on a diagnostic rather than on a mock expectation that could
  never be met. The selection resolves once per process inside `LibCCEC::init`.
- Each declared observable difference between the back-ends is a pair of behaviours, established
  only by asserting both halves. The legacy halves live here, on invocation A; the AIDL halves live
  in the invocation-B fixtures, because one process can hold only one back-end. The pairing turns
  "the AIDL back-end raises" into "the two back-ends differ in exactly this way and nowhere else".
- The shared driver is used rather than a local `DriverImpl` because the legacy arms need an open
  driver, and the shared one is the only driver in the process whose HAL is the mock the fixture
  programs. The local-instance fixture opens no local `DriverImpl`, and its local `DriverAidlImpl`
  instances are either plain and CLOSED or probes whose forced or injected OPENED state their
  destructors return to CLOSED. An arm reachable only after the legacy prelude and guard have
  passed on an open legacy driver therefore cannot be reached from that fixture.
- The receive-path case is not a difference arm: both back-ends must deliver an inbound frame
  identically, so its legacy half is a baseline, measured here because this is the only invocation
  that runs on a host without a binder driver.
- Self-sufficiency: SetUp establishes the open precondition itself instead of inheriting what the
  previous suite left, each case programs its own mock expectations and TearDown clears them, and
  the driver is left open, the state the global environment set up.
- SetUp: both checks are fatal, because a body run against a closed driver or the wrong back-end
  would report a harness failure as a middleware one. `open()` is not wrapped in `catch(...)`: it
  returns silently when the driver is already opened (its InvalidStateException throw is compiled
  out), so its only possible exception is the IOException meaning the mock HAL refused to open.
  Swallowing it would leave the process-global driver CLOSED for this fixture and every sibling
  suite in the binary; as a fatal setup failure the body does not run and TearDown still restores
  the shared state.
- TearDown: the restore is a non-fatal expectation, so a failure is reported without cutting the
  rest of the cleanup short, and no sibling suite observes a closed driver because of this fixture.
- The mock member: unlike the local-instance fixture, cases here set expectations on it, because
  the legacy arms are exactly the ones that must reach the legacy HAL.

### DriverAidlLegacyArmTest.PhysicalAddressIsReadThroughTheLegacyHalApi

- The legacy back-end reads `HdmiCecGetPhysicalAddress` and delivers its value; the out-parameter
  is seeded with `0xDEADBEEF`, which must be gone afterwards, so a case that left it untouched
  cannot pass. The HAL value `0x1000u` is a distinctive test value, not the 1.0.0.0 encoding.
- Superseded statements: the earlier comment called this "the legacy half" of a physical-address
  requirement whose AIDL half was blocked on B1, named the AIDL case asserting that interim
  behaviour as its pair, and described `0x1000` as 1.0.0.0 so the case stayed distinguishable from
  the blocked AIDL behaviour. The AIDL back-end now reports a fixed 1.0.0.0 (`0x01000000`, one
  nibble per byte) in every driver state without any HAL call, so it is not blocked, and `0x1000`
  does not decode as 1.0.0.0 in that encoding. This case's assertion, the legacy HAL read, is
  unchanged.

### DriverAidlLegacyArmTest.FramesUpToTheLegacyMaximumAreSentOnTheLegacyBackEnd

- The three sizes straddle an authority conflict rather than sampling a range: a `CECFrame` carries
  up to 128 bytes, the legacy HAL specification allows 20 and the AIDL `sendMessage` contract
  states 16. Each is authoritative for its own back-end, so 17 to 20 is a disputed band, accepted
  here and refused by the AIDL back-end. 16 is the control both must accept, which confines the
  difference to the band rather than to the existence of a guard.
- The captured length is asserted per size because the claim is not that the call happened but
  that the whole frame reached the HAL un-truncated, the property the AIDL half refuses to
  compromise by truncating.

### DriverAidlLegacyArmTest.WriteAsyncSucceedsOnAnOpenLegacyDriver

- The AIDL back-end refuses this arm with OperationNotSupportedException, and it is the only part of
  `writeAsync` where the two differ: the prelude ordering and state guard are identical on both and
  asserted in `DriverAidlLocalInstanceTest`, so with this control the divergence is one branch wide
  rather than diffuse.
- No production call site reaches `writeAsync` on either back-end: every plugin transmit goes
  through `Connection::sendToAsync` and `Connection::sendAsync` onto the Bus writer thread, which
  calls the synchronous `write`. It is asserted anyway because "unreachable today" is not
  "impossible".
- It needs an open driver, which is why it cannot live in the invocation-independent fixture.

### DriverAidlLegacyArmTest.LegacyTransmitStatusTranslationDistinguishesDirectedFromBroadcast

- Measured so the AIDL translation has a measured baseline to mirror. The two arms are the ones the
  inverted AIDL sense makes easy to get wrong: in `DriverImpl::write`, a directed frame reported as
  not acknowledged raises CECNoAckException, while a broadcast reported the same way returns
  normally unless it is the CEC CTS 9-3-3 opcode (REPORT_PHYSICAL_ADDRESS), which raises so the
  caller retries at least once. In `DriverAidlTransmitTest`, ACK_STATE_0 and ACK_STATE_1 swap
  meaning between the two destinations.

### DriverAidlLegacyArmTest.ReceivedMessageReachesAnApplicationListenerOnTheLegacyBackEnd

- The receive path is not a declared difference: a frame the HAL delivers must reach an application
  listener byte for byte on both back-ends. The AIDL claim (copied into a fresh `CECFrame`, offered
  through the state-guarded accessor, drained by the Bus reader, delivered through `Connection`'s
  filter to a `FrameListener`) is meaningful only against this measured legacy baseline, asserted
  on the same helpers with the same bounded wait so the halves are comparable rather than adjacent.
- It also exercises the observation machinery on any host. The AIDL receive cases live in an
  invocation-B fixture and need a binder driver, so a miswired recording listener or listening
  connection would fail all of them on the binder-capable runner for a reason unrelated to the
  middleware.
- The frame is injected through the mock's captured Rx callback, the route the end-to-end
  integration cases in `ccec/test_Connection.cpp` use: the vendor HAL's own delivery mechanism,
  invoked on the caller's thread here rather than on a HAL thread.

### DriverAidlSessionFixture

- Not a suite of its own (no `TEST_F` names it), so it adds no case to the filter surface; it holds
  the non-trivial precondition both invocation-B fixtures need, kept in one copy.
- Requires invocation B (`CEC_TEST_AIDL_MODE=compatible`, on a host with a binder driver and a
  running service manager). SetUp fails rather than skips when the AIDL back-end is not resolved:
  a skipped arm is indistinguishable from a passing one in an aggregate count, which would let an
  AIDL invocation report green having never exercised the AIDL path. It does not read
  `CEC_TEST_AIDL_MODE`; only the harness does (`applyAidlModeBeforeInit()` and
  `publishFakeForMode()` in `tests/L1Tests/test_main.cpp`). Branching on it would make a mis-set
  variable and a mis-wired filter indistinguishable.
- Why close -> reset -> open: `FakeHdmiCecService::reset()` clears the captured listener, returning
  the event triggers to no-ops, and the listener is captured only by `IHdmiCec::open()`, which
  `DriverAidlImpl::open()` does not call when the driver is already OPENED (it returns silently).
  Resetting without re-opening would leave every trigger a silent no-op, and a delivery case would
  fail for a reason unrelated to the code under test. The order leaves each case a clean fake, a
  captured listener and counters that describe only SetUp's own open.
- Closing the shared driver is safe by design: `close()` offers the NULL sentinel, which wakes the
  blocked Bus reader, and `Bus::Reader::run()` catches InvalidStateException and stays in its loop,
  so it re-arms the moment `open()` restores the driver; the reader runs throughout. Reaching the
  same state through `LibCCEC::term()` and a re-arming `init()` is avoided: the middleware's own
  test notes identify that pattern as the reader-thread hazard, and the neighbouring async suite
  (`test_DriverImpl_Async.cpp`) records the analysis.
- Self-sufficiency: every case gets a freshly reset fake and an open session, and TearDown resets
  the fake and its controller before restoring the opened baseline non-fatally. The reset comes
  first because a case that installed a failing open or close status would otherwise make the
  restoration fail for a reason the case created. A failing case therefore leaves neither a closed
  driver nor a configured fake behind, here or in a sibling suite.

### DriverAidlSessionTest.CompatibleServiceSelectsTheAidlBackEndAndNamesItInTheLog

- It uses the same two independent routes as the legacy selection case (concrete type in both
  directions, and the selected-path log line), so the pair is symmetric. The log route is the one
  the coverage runner and device-level validation have, since the Driver interface carries no
  introspection API.

### DriverAidlSessionTest.AddLogicalAddressMarshalsExactlyOneElement

- The AIDL calls take `int[]` while the middleware API is single-valued throughout, so the array
  shape must stay confined to the temporary the adapter builds: no multi-address state, no
  iteration, no fan-out, and no middleware structure or signature growing a plural form. Asserting
  `size() == 1` and the value distinguishes "one address was sent" from "an array happened to
  arrive"; a value-only check would pass against an implementation that padded the vector.
- The local effect is asserted because it is the caller-visible half: `Connection` consults the
  locally held address, one address at a time.
- Separate add and remove counters cannot show which call came first. So the case captures
  stdout across the add and requires the fake's `removeLogicalAddresses` line to precede its
  `addLogicalAddresses` line. Each line is logged as its call runs, and an in-process call
  dispatches directly, so the captured order is the order the HAL saw.

### DriverAidlSessionTest.RemoveLogicalAddressMarshalsOneElementAndIgnoresHalRefusal

- The legacy shape (`DriverImpl::removeLogicalAddress`) is the specification and is reproduced
  rather than improved: the state guard, then the removal from the local list, and only then the
  HAL call, whose return value is discarded. A HAL that declines the removal therefore changes
  nothing: the address is gone locally either way and nothing is raised. Asserting the local effect
  after a declined removal pins that order; asserting only "no exception escaped" would pass
  against an implementation that rolled the local removal back. Raising would be an unregistered
  behaviour change that Bus and LibCCEC do not expect.

### DriverAidlSessionTest.RemoveLogicalAddressSucceedsMarshalsOneElementAndDropsItLocally

- The success arm is the one the coverage branch manifest names an invocation-B reacher for. The
  declined and transport-failure arms both establish that a failure is discarded and leave
  `ok == true` uncovered: an implementation that always took the failure path (logging, never
  treating the removal as successful) would satisfy every other case. Four assertions together
  describe a success: exactly one HAL call, a one-element vector carrying the right value, no
  exception, and the address gone locally.
- The result is set explicitly so the arm under test is stated in the case rather than inferred
  from the fake's initial state, which a change to that state would silently move.

### DriverAidlSessionTest.AFailedCloseStillReleasesAReaderParkedOnTheIncomingQueue

- What it protects: `DriverAidlImpl::close()` offers the NULL sentinel immediately after moving the
  state to CLOSING, and only then performs the HAL transaction and evaluates its result, raising
  IOException if either says the close did not happen. Moving the offer below the
  `if (!txn.isOk() || !closed) throw` means the failure path never offers the sentinel, while the
  state still reaches CLOSED, the exception still escapes, the local address list is still kept and
  every call count is still right; a thread parked in the queue's blocking poll then stays parked
  forever, because the sentinel is the only thing that wakes it. A middleware whose reader thread
  never returns cannot be torn down or re-initialised.
- Three properties frame the observation. (1) The session is proved live first, by a successful
  write, so the first `read()` cannot end at its entry guard. (2) The first read completes on a
  delivered frame and the reader then re-enters `read()`; its counter is raised before that
  second call, so the failing close may come before the second read starts, in which case the
  entry guard rather than the sentinel ends it. (3) The termination cannot be explained by later
  cleanup: the bound is waited out while the driver is alive and in scope, and the termination
  must be InvalidStateException, which both the sentinel and the entry guard produce once the
  state is no longer OPENED. The case therefore asserts that the reader terminates, not which of
  the two ended it, so a misplaced sentinel offer is caught on runs where the second read reached
  the blocking poll before the close.
- A local instance is used, not the process-global driver: the global one belongs to the Bus
  reader, and a second consumer on that queue would compete for one sentinel. Under invocation B a
  local instance resolves its own proxy through `isServiceAvailable()` and opens against the same
  fake, so it has a queue of its own with exactly one reader.
- Both failure arms (the HAL reports it did not close; the close transaction fails) are covered
  because they converge on one condition and each can satisfy it alone; `Arm` tabulates them so
  the two cannot drift apart.
- The driver and reader state are heap-held: if the sentinel is missing the reader cannot be
  joined, and abandoning the thread is the only alternative to hanging the run, which is safe only
  if everything it touches outlives the scope. Every assertion after the reader starts is non-fatal
  so the scope is never left with an unjoined thread. On failure a clean re-open and close offers a
  fresh sentinel as a best-effort rescue; if even that leaves the reader parked, the thread is
  detached (it shares ownership of the driver and its own state) and the binary's later results are
  flagged as suspect.

### DriverAidlSessionTest.GetLogicalAddressReadsEntryZeroFromTheServiceInterface

- `getLogicalAddresses` lives on `IHdmiCec`, while only `addLogicalAddresses`,
  `removeLogicalAddresses` and `sendMessage` live on `IHdmiCecController`. An earlier specification
  attributed the call to the controller, so the service-side call counter pins where the call goes.
  A call routed to the controller would not compile against this interface snapshot, so a zero
  count means the address came from somewhere else entirely.

### DriverAidlSessionTest.MultipleReturnedAddressesUseEntryZeroAndAreLogged

- A multi-address HAL result is logged and its first entry used, with no multi-address state, no
  iteration and no dispatch fan-out added to the middleware. Both halves (the returned value and
  the log naming the count and the entry used) are asserted, because the log is the only signal a
  platform integrator gets that the HAL reports more addresses than the middleware can represent,
  and a silent first-entry pick would hide a real configuration problem.
- The log half matches the composed line, not a digit: every `CCEC_LOG` line is prefixed with a
  `YYMMDD-HH:MM:SS:usec` timestamp, so a bare "3" (the vector's count) is present in any non-empty
  capture and would hold against a log that never mentions the count. The expected text is built
  from the vector's size and entry 0 and matched as one contiguous string, so a digit can satisfy
  it only next to the label that gives it meaning; building it from the vector also keeps
  expectation and stimulus aligned if the addresses change.
- The expected text is transcribed from the format string inline in
  `DriverAidlImpl::getLogicalAddress()`, which cannot be imported:
  `"DriverAidlImpl::getLogicalAddress : the HAL reports %zu logical addresses; operating on entry 0 [%d]\r\n"`,
  with `halAddresses.size()` and `(int)halAddresses[0]` substituted. The terminator is left out
  because the line is matched as a substring of a capture that also holds the CEC log prefix, the
  timestamp and anything else emitted while the capture was open.
- One match covers both required parts of the report (the count the HAL returned and the entry
  used), each next to its own label; two loose fragment searches would report which half is missing
  but could each be satisfied by text the report did not produce.
- No statement in this slice about logical addresses was superseded by the DeviceType-derived
  registration at enable: `getLogicalAddress` still reads entry zero of
  `IHdmiCec::getLogicalAddresses()` on every call, and these cases program the fake's address
  result directly.

## tests/L1Tests/ccec/test_DriverAidl.cpp (part 6 of 6)

Detail moved out of the comments on the remaining `DriverAidlSessionTest` cases and on the
`DriverAidlTransmitTest` fixture and cases. `path:line` references are as of the PR that
introduced the AIDL back-end.

- **Superseded:** the pre-refine comments framed the AIDL/legacy differences as a closed list of
  exactly three "authorized differences" (17–20-byte frames, `writeAsync`, and the
  `addLogicalAddress` failure category), so that clearing the address list at close would be "a
  fourth observable difference" and any unlisted raise "an unregistered difference". The AIDL
  back-end now also discovers and registers one DeviceType-derived logical address when `open()`
  reaches OPENED, so that closed-list framing no longer holds; the entries below record the
  behaviours without it.

### DriverAidlSessionTest.EmptyAddressResultReportsZeroSoTheExistingCallerSignalSurvives

- The AIDL contract states the `getLogicalAddresses` array is empty when no additional addresses
  have been set, so an empty result is a documented normal outcome, not an error: 0 is the
  contract rather than a shortfall.
- Reporting 0 keeps the AIDL path behaving like the legacy path: `LibCCEC::getLogicalAddress`
  turns a zero into `InvalidStateException` (`LibCCEC.cpp:164-167`), the signal callers already
  handle. Any other sentinel would suppress that throw and hand the caller an address it does not
  hold.

### DriverAidlSessionTest.AddLogicalAddressMapsRefusalAndTransportFailureToDistinctExceptions

- The failure category is coarser on the AIDL back-end than on the legacy one, and that is forced
  rather than chosen.
- The legacy implementation distinguishes three outcomes from one HAL status
  (`DriverImpl.cpp:339-347`): `LOGICALADDRESS_UNAVAILABLE` becomes `AddressNotAvailableException`,
  `GENERAL_ERROR` becomes `IOException`, and any other value is success.
- `IHdmiCecController::addLogicalAddresses` returns one boolean, documented as false when the
  address is outside 0x0..0xE or already added. Carrying the distinction would need a new HAL
  method, which the constraints forbid, so `false` maps to the nearer legacy category
  (`AddressNotAvailableException`) and a non-ok binder status maps to `IOException`.
- Both mappings are asserted here; their caller-visible effect is measured by the following case
  rather than assumed equivalent.
- In-body: a refusal models an address that is out of range or already taken; a transport failure
  models a dead binder, an `EX_ILLEGAL_STATE` or a marshalling fault.

### DriverAidlSessionTest.AddLogicalAddressFailuresReachBothRealSinkCallPathsAsExpected

- Measured through the real caller paths rather than the `Driver` interface, because "the caller
  sees the same thing" is a claim about the plugin, not about the adapter.
- The two Sink call paths differ structurally (re-read from the plugin source, which is out of
  scope and not modified):
  - Path 1: `LibCCEC::getInstance().addLogicalAddress(...)` at
    `entservices-hdmicecsink/plugin/HdmiCecSinkImplementation.cpp:2767`, inside the try opened at
    `:2764`, which has three catches — `catch(InvalidStateException &e)` at `:2789`,
    `catch(IOException &e)` at `:2793` and `catch(...)` at `:2797`. A transport failure reaches
    the distinct `:2793` arm; a refused address falls through to the generic `:2797` arm. Both set
    the same poll-thread state, but they are different arms and log differently.
  - Path 2: the enable-time call at `:3065`, inside the `if` at `:3061` and inside no try block
    (the nearby `IOException` catches at `:3040` and `:3173` belong to `LibCCEC::init` and `term`),
    so either exception propagates out of the enable path.
- `LibCCEC::addLogicalAddress` itself catches nothing and propagates (`LibCCEC.cpp:133-144`). Both
  dispositions are modelled: a try/catch shaped like path 1 and an uncaught propagation shaped
  like path 2; on path 2, `EXPECT_THROW` asserts that nothing in the middleware swallows the
  exception on the way up.
- What this establishes: the coarser category still lands in a specific, handled arm on path 1 and
  still propagates on path 2, which is what makes the difference tolerable for those callers.
- Drift guard: `DriverAidlLocalInstanceTest.TheModelledSinkCallPathsStillMatchTheRealSinkSource`
  reads the plugin source and asserts the three structural facts this case depends on — two call
  sites, the first inside a try carrying both an `IOException` arm and a generic arm, the second
  inside no try. It runs on every invocation, including those with no binder driver, so it is
  present on runs where this case is filtered out. If the plugin drifts, that case fails and names
  what changed; this one keeps asserting the behaviour, which is the middleware's to guarantee
  whatever the caller does with it.

### DriverAidlSessionTest.FailedCloseStillReachesTheClosedStateAndKeepsTheLocalAddressList

- Asserted as a sequence rather than an outcome. Three ordered, observable properties of
  `DriverAidlImpl::close()`:
  1. The state becomes CLOSED before any failure is raised (mirroring `DriverImpl.cpp:145-150`),
     so a caller that swallows the exception still sees a consistently closed object rather than
     one that believes it is open with no session behind it.
  2. The local logical-address list is deliberately not cleared: `DriverImpl::close()` does not
     clear it either, so `isValidLogicalAddress` can keep reporting true across a close, and the
     HAL drops the addresses on its own side anyway.
  3. The controller reference is released, so a subsequent operation cannot reach a stale session.
- Asserting only that `IOException` escaped would leave all three unproved.
- Controller identity is asserted, not merely that some close happened. The fake captures the
  controller before evaluating its configured result, so identity holds on the failure path; a
  close naming the wrong controller leaves the real session open HAL-side while the middleware
  believes it has gone, which is exactly the state `IHdmiCec::open()` then refuses with
  `EX_ILLEGAL_STATE`.
- CLOSED is observed through a guarded operation (`write()` raising `InvalidStateException`),
  because the state is private and no introspection exists; that is the caller-visible form of
  the invariant. A second close returns silently, as the legacy back-end does with its throw
  compiled out.
- A green result does not confirm B2: which AIDL method corresponds to the legacy
  `HdmiCecClose()` is pending owner confirmation, and the asserted sequence holds whichever method
  the owners confirm.

### DriverAidlSessionTest.CloseWithNonOkBinderStatusAlsoReachesTheClosedState

- A non-ok binder status on close is a different arm from a `false` result, and it must reach the
  same closed state.
- Both arms converge on one condition in `DriverAidlImpl::close()` and either can satisfy it alone,
  so covering only one would leave a short-circuit reordering — one that skips the CLOSED
  assignment on the transport arm — undetected.
- The fake captures the controller before the transaction status is applied, so identity is
  observable on this arm too; a transport failure is when a mis-named session matters most,
  because nothing HAL-side has changed.

### DriverAidlSessionTest.ReceivedMessageIsAcceptedWhileTheDriverIsOpen

- End-to-end chain: HAL event, adapter, incoming queue, Bus reader thread, Connection's filter,
  application listener; the bytes that arrive are the bytes that were sent.
- Why the delivery and not the trigger: `FakeHdmiCecService::fireOnMessageReceived` returns false
  only when no listener was captured and reports nothing about what the listener then did. "The
  trigger returned true" is satisfied by an adapter that allocates the frame and drops it, by one
  that offers it onto a queue nothing drains, and by one whose state guard wrongly rejects it —
  three defects that each break the receive path completely. Only an application-listener
  notification carrying the exact bytes distinguishes them. Every step between is load-bearing:
  the callback copies the bytes into a fresh `CECFrame` and offers it through the state-guarded
  incoming-queue accessor, and the Bus reader is blocked on that queue.
- The wait is a bounded condition-variable wait, not a sleep. Delivery is asynchronous even here,
  because the Bus reader is a separate thread blocked in the queue's poll: the adapter offers on
  the calling thread and the reader wakes and notifies. A fixed sleep would be flaky or wasteful
  and assert nothing; the wait returns the moment the frame arrives and fails at the deadline.
- Not established here: the callback itself runs on the calling thread, because an in-process
  fake resolves to the local `BBinder` and no transaction crosses the binder driver. Delivery of a
  callback arriving on a genuine binder threadpool thread is invocation E's, over real IPC, in
  `tests/L2Tests/ccec/test_DualPathIntegration.cpp`.

### DriverAidlSessionTest.AZeroByteMessageFromTheHalIsDiscardedBeforeAllocation

- The alternative to discarding is a dead process, not a dropped frame. A `CECFrame` with no bytes
  cannot be decoded: every consumer first reads byte zero for the header nibbles, and
  `CECFrame::at()` raises `std::out_of_range` when there is none. Neither `printFrameDetails()`
  nor `Bus::Reader::run()` catches that, so it escapes the reader's thread function.
- The guard therefore sits ahead of the allocation and ahead of the queue, and an empty payload is
  the only input that reaches it — no canned status, binder failure or state can.
- Why it was not covered before: nothing in the middleware produces a zero-byte message; only a
  HAL can, which is why the guard exists and why a fake is the only way to drive it. On a host
  with no binder driver the AIDL back-end is never selected and this callback never runs, and the
  runs that do select it had no case delivering an empty payload.
- Three assertions, the second separating "discarded" from "queued and undecodable":
  1. Nothing escapes the oneway callback; an exception reaching `onTransact` would be worse than
     dropping the frame, which is also the legacy callback's disposition.
  2. No frame is delivered, waited for over a bounded window rather than checked at one instant:
     a queued zero-byte frame would be drained by the reader and raise inside it.
  3. The delivery chain is alive throughout, shown by a positive control (a well-formed frame one
     byte longer) after the empty payload; without it a dead receive path would read as a correct
     discard.

### DriverAidlSessionTest.AnOverLengthMessageFromTheHalIsDiscardedWithoutEscaping

- The arm where the two back-ends differ in consequence, not merely in degree. `CECFrame::append()`
  takes bytes one at a time and raises `std::out_of_range("Frame grows beyond maximum")` on the
  byte past the capacity (`CECFrame.cpp:54-64`). On the legacy back-end that append is outside
  `DriverImpl::DriverReceiveCallback`'s try block (`DriverImpl.cpp:60`), so the exception escapes
  the HAL's callback and takes the process down — measured, identically on the pre-migration base,
  so it is a pre-existing condition of that back-end and out of scope to fix. The AIDL back-end
  puts the allocation and the append inside the try, and the general catch arm deletes the frame
  and returns `binder::Status::ok()`. Containment is a deliberate property of the new code, and
  this case keeps it exercised.
- Why it was not covered before: nothing in the middleware can produce a message this long (a
  `CECFrame` cannot hold one, and the outbound guard refuses anything past the AIDL contract's
  length), so only a HAL — here a fake — can deliver one; on a driverless host this callback never
  runs.
- One case over two sizes, not two cases and not a `TEST_P`: 129 and 4096 bytes are two points on
  one arm, so the reasoning is stated once and `SCOPED_TRACE` names the failing size; the transmit
  half of the guard (`FramesOverTheAidlLimitAreRefusedWithoutBeingSentOrTruncated`) has the same
  loop shape; nothing in the file is value-parameterized. Every per-size precondition is non-fatal
  (`ADD_FAILURE()` then `continue`), so one size cannot leave the other unmeasured.
- Sizes are derived from `CECFrame::MAX_LENGTH`, never restated. Capacity + 1 is the boundary
  whatever the capacity becomes, and the only size an off-by-one in the copy would show at;
  32 × capacity is the arbitrarily large delivery an out-of-process HAL may make, the size class
  at which an ad-hoc guest harness once demonstrated containment without leaving a regression
  guard behind. The `MAX_LENGTH` `static_assert` at
  the top of the file guards the transmit side's 16/17/20 authority conflict, not this question,
  so this case does not lean on it.
- Four assertions per size, the first separating this back-end from the legacy one:
  1. Nothing escapes the callback. The fake invokes the listener directly and catches nothing
     (`fake_hdmi_cec_aidl_service.cpp:1063-1085`), and under this invocation the dispatch is local,
     so `EXPECT_NO_THROW` is a real observation of the property the legacy back-end lacks.
  2. No frame is delivered over a window. `append()` copies `MAX_LENGTH` bytes before it raises, so
     what is ruled out is a silently truncated frame, which every consumer above the queue would
     treat as ordinary and a peer could read as a different message entirely.
  3. The listener holds nothing afterwards, which catches a frame that surfaced after the window.
  4. The chain is alive, via a positive control carrying the same header byte. The connection
     filters nothing for an UNREGISTERED source (`DefaultFilter::isFiltered`,
     `Connection.cpp:304-306`), so payload and control differ only in length.
- The payload is the control's bytes padded to the target size, so its first `MAX_LENGTH` bytes
  are a frame the whole chain would accept, which makes a truncated delivery detectable. Each size
  uses a fresh listener and connection, so counts start from zero.
- The middleware's log line is deliberately not asserted: this arm emits the general catch arm's
  line, which an allocation failure also emits, and moving the rejection ahead of the allocation
  would change that text while every asserted property stayed true.

### DriverAidlSessionTest.AFrameArrivingWhileTheQueueIsFullIsReleasedRatherThanLeaked

- `EventQueue::offer()` returns void and silently discards its argument once the queue is full, so
  a callback that offered and then dropped its pointer would leak one heap `CECFrame` per event,
  without bound, while the reader stayed behind. `DriverAidlImpl::offerReceivedFrame()` reports the
  refusal instead and the callback releases the refused frame; that release is the arm.
- Filling the queue needs a blocking listener. The reader normally waits in `EventQueue::poll()`
  on an empty queue and drains as fast as frames arrive, so occupancy never exceeds one. Parking it
  inside a notification, on its own thread and with nothing injected into the queue, produces the
  real slow-application condition.
- Production numbers are not restated: the refusal point is the queue's capacity. The case
  delivers 64 frames, comfortably past it whatever that constant is, and asserts on the refusal
  line rather than on a computed occupancy.
- The refusal is observed in the log because a released allocation has no other visible trace; a
  leak assertion would need a heap harness the suite does not have.
- Two substrings are asserted — the condition ("refused the frame at its") and the disposition
  ("rather than leaking it") — because a line reporting the refusal without the release would pass
  a single check, and an unreleased refused frame is a leak per event.
- `StdoutCapture::isValid()` is sampled before `read()`, because `read()` calls `restore()` and a
  restored capture reports itself invalid; asking afterwards always answers false (an earlier
  revision fell into this and the guest run caught it). `read()` returns an empty string rather
  than raising when redirection failed.
- The reader is released before any assertion, so a fatal one cannot leave it parked in the
  listener and hang teardown. A final delivery shows the receive path recovered rather than wedged.

### DriverAidlSessionTest.ASynchronousCallPastTheSlowThresholdIsReportedWithoutChangingItsResult

- The slow-call diagnostic is a threshold, not a timeout: crossing it abandons nothing, raises
  nothing and changes no return value; it leaves one `LOG_WARN` line where a stall would be silent.
- B4 records that the underlying synchronous binder call cannot be bounded on the pinned
  libbinder, so this line is the entire in-scope mitigation, and it is asserted directly.
- No canned result or status takes time, so only a genuinely slow call reaches the arm; hence the
  fake's delay knob (`FakeHdmiCecController::setAddLogicalAddressesDelayMs()`) and the real
  wall-clock cost. The delay is just past the threshold (`kSlowHalCallWarnMs` + 150 ms) to cost
  the least, and is cleared immediately so no later case inherits it.
- The production threshold lives in an anonymous namespace in `DriverAidlImpl.cpp` (line 185) with
  no dynamic symbol, so it is restated in `kSlowHalCallWarnMs`. The restatement fails safe: if
  production raised its threshold above this value, no line would be emitted and the case would
  fail loudly rather than quietly stop testing the arm.
- Asserted: the call (`DriverAidlImpl::addLogicalAddress()`) still succeeds unchanged — a
  diagnostic altering the outcome it describes would be far worse than a missing line — and the
  line contains "past the", "NO DEADLINE WAS ENFORCED" (so it cannot be mistaken for a bounding
  mitigation) and the operation name `addLogicalAddresses`. The elapsed figure is not asserted;
  pinning a real shared-machine measurement would add flakiness without evidence.
- `StdoutCapture::isValid()` is sampled before `read()` for the reason given in the queue-refusal
  case.

### DriverAidlSessionTest.MessageArrivingWhileClosedIsRejectedAndReleased

- This arm stops a frame leaking per rejected message and stops a late frame reaching an
  application that has torn its session down.
- The adapter reaches the queue through a state-guarded accessor that raises when the state is not
  OPENED, exactly as the legacy receive callback does, and the listener catches that and frees the
  allocation. Offering directly to the queue member would accept frames the legacy path rejects
  and leave them in a queue nothing drains until the next open.
- Three assertions, the third making this more than a smoke test:
  1. Nothing escapes the binder callback: a oneway callback has no caller to receive a fault, so
     reaching `onTransact` would be worse than dropping one frame, the legacy disposition too.
  2. No delivery while closed, over a bounded window, because "it had not arrived yet" and "it
     will never arrive" are different claims.
  3. No delivery after a re-open. A frame wrongly offered while closed would sit behind the close
     sentinel, and re-opening re-arms the Bus reader, which would drain and deliver it to an
     application that believes the session was down. Only the re-open can detect that.
- Two logged dispositions are both correct: "Exception during frame offer...discarding" (the
  accessor raised and the frame was released) or "message received after detach, dropping it"
  (the listener had already been detached by `close()` and dropped the message before allocating).
  Silence would mean the frame was accepted into the queue — a divergence from the legacy path.
- The fake retains the listener's binder reference after `close()`, which models the real hazard:
  a HAL that calls back after the session went down. A distinctive frame is used so a delivery
  cannot be a frame left over from another case.
- The release is observed in the middleware's log at a level the default configuration prints.
  The positive control after the re-open rules out a chain dead for an unrelated reason — a
  withdrawn listener, a stopped Bus reader, or a filter rejecting everything. TearDown restores
  the opened baseline.

### DriverAidlSessionTest.DiagnosticCallbacksAreReportedWithoutDisturbingTheSession

- The claim has two halves: what is logged and what is not done. `onStateChanged` and
  `onMessageSent` are logged and acted on in no other way, because neither has a legacy
  counterpart and acting on them would be new behaviour. A transition to CLOSED does not offer the
  close sentinel onto the incoming queue (an in-process HAL cannot vanish, so the legacy path has
  no such notion), and no death recipient is installed for the same reason.
- "Does nothing" is not "is not implemented": a later transmit still working cannot tell a correct
  callback from an empty body, so the diagnostic output itself is asserted.
- `DriverAidlImpl::EventListener::onStateChanged()` logs at `LOG_INFO`, which `cec_log_level`
  defaults to, so its line always prints. Both state names go through the generated `toString()`
  and both are asserted; the old state is what distinguishes a transition from a repeated
  notification.
- `DriverAidlImpl::EventListener::onMessageSent()` logs at `LOG_DEBUG`, suppressed by default, with
  three fields — the `SendMessageStatus` name, the message length and the message bytes — all
  asserted, since together they say which outcome the bus reported and which in-flight message it
  concerned. Nothing else records that outcome, because `write()` acts on `sendMessage()`'s return
  value, not on this callback.
- Bytes are rendered as lowercase hex, two digits per byte and no separator, into a stack buffer
  sized from `CECFrame::MAX_LENGTH`, with "..." appended past it — a bound rather than an
  allocation, because the callback runs on a binder thread. `lowercaseHexOf()` reproduces that
  rendering for the four-byte message used here.
- The length is matched with its label, transcribed from the producer's format
  `"... Result: %s, message length: %zu, message bytes: %s"`: a bare
  `std::to_string(sentMessage.size())` is one digit and every captured `CCEC_LOG` line carries a
  timestamp, so that form could never fail.
- Raising the level: `ScopedCecLogLevel` writes a level name to `/tmp/cec_log_enabled` (the path
  `check_cec_log_status()` hardcodes) and calls that reader — the only seam; there is no setter
  for `cec_log_level`. The path is shared by every process on the host, so the guard's custody
  protocol applies: an exclusive lock before anything is captured, refusal of a symlink or a
  foreign owner, and an atomic replace rather than a truncating write. It refuses rather than
  forces whenever the path is not safely the run's to modify.
- The raise is asserted, not reported: a refusal hides `onMessageSent`'s line, and skipping its
  assertions would let missing evidence pass as evidence. The failure message streams the guard's
  reason, distinguishing an environment fault (path not writable, directory gone) from a custody
  refusal (another `run_L1Tests` holds the lock, the path is a symlink or foreign-owned, a
  concurrent writer replaced it).
- Ordering inside the case: the level is raised before the capture opens, so neither the guard's
  custody work nor its level probe lands in the captured text, and its verdict and reason are
  copied while the guard is alive. The capture closes in an inner scope before restoration,
  because `restoreAndVerify()` re-probes the level by logging through `CCEC_LOG`, which would
  otherwise be appended to the captured text and be indistinguishable from callback output.
- Restoration is asserted on the normal path while the lock is held. `restoreAndVerify()` proves
  that publishing the level and the file succeeded (every syscall result checked), that the path
  holds the captured bytes again (re-read without following a link) or is absent again where the
  guard created it, and that the process-wide level is the one found on entry — which a restored
  file does not imply, because `check_cec_log_status()` leaves the level alone when the file's
  first line matches nothing in its table. The destructor is a backstop for a fatal unwind; it
  cannot fail a test, so a failed write, rename or unlink there would leave DEBUG in force or the
  shared file altered while every case still passed.
- The level-independent half matters most: neither callback puts anything on the receive path (no
  sentinel on CLOSED; a transmit report is not an inbound frame), and the session still transmits.

### DriverAidlSessionTest.OpenSucceedsAndDeliversWhenAThreadPoolWasAlreadyStarted

- `DriverAidlImpl::open()` calls `ProcessState::self()->startThreadPool()` unconditionally, and
  that is the whole obligation: the call is idempotent, and `setThreadPoolMaxThreadCount` is not
  called because no back-end-local flag can detect a pool another component started and lowering
  an established maximum can abort the process. A naive local flag would break the
  already-started case.
- By the time this case runs a pool has certainly been started (by the open in `LibCCEC::init` and
  by SetUp's), so the already-started condition is exercised. The never-started condition is the
  process's first open inside `LibCCEC::init`; a failure there aborts initialization and no case
  runs.
- Frames alone cannot prove a pool: the in-process fake runs the callback on the calling thread,
  so frames arrive whether or not a pool was started, and the omission would surface only on a
  device as a receive path that never delivers.
- The two outside-the-process observations are unsound on this SDK:
  - Thread name: `ProcessState::makeBinderThreadName()` composes "binder:PID_N", but
    `androidCreateRawThreadEtc()` discards it — its `threadName` parameter is `__android_unused`,
    and the block that would apply it, with the only `androidSetThreadName()` call site for a
    spawned thread, is under `#if defined(__ANDROID__)`, which the SDK's CMake build never
    defines. Measured: every thread of the runner and of the fake service host carries its own
    process name.
  - Thread count: it moves during a run as cases start and join their own workers (measured
    1 → 5 → 6, with different thread ids at equal counts), so inferring the pool from a count, or
    from a count that does not grow on a redundant `startThreadPool()`, is unsound.
- So libbinder is asked: `getThreadPoolMaxThreadCount()` returns the configured maximum once
  `mThreadPoolStarted` is set and zero while it is not — the flag `startThreadPool()` sets, via a
  public API, identical on both platforms. It is read before the session is touched, so it is
  evidence about production's own open during `LibCCEC::init`, not a pool this case caused.
  `selfOrNull()` is used so the probe cannot create a `ProcessState`; null means no pool can exist
  and the back-end never reached libbinder.
- The thread-name probe is printed rather than asserted, as corroboration on a build that applies
  the name; a miss is the ordinary case on this port.
- Real-thread evidence is invocation E's: a `oneway` callback received on a pool thread over real
  out-of-process IPC (`tests/L2Tests/ccec/test_DualPathIntegration.cpp`). The two tiers together
  discharge the obligation, since a pool with no callback and a callback with no pool are both
  consistent with a broken device receive path; this case owns the idempotency half.
- The close/open cycle makes `open()` run its body again. The successful close is asserted to
  name the controller `open()` returned (both failure arms are covered by the two close cases),
  because `IHdmiCec::close` takes the controller precisely so the HAL knows which session ends.

### DriverAidlSessionTest.OpeningAnAlreadyOpenDriverIsASilentNoOpThatTouchesNothing

- `open()` on an already-OPENED driver returns without doing anything, preserving the legacy
  behaviour: `DriverImpl::open()`'s throw for this case is compiled out, so the observable legacy
  behaviour is a silent return. Raising here would also be hit by `Bus::start()`.
- "Returns silently" and "re-opened the session" are indistinguishable unless the HAL is asked.
  Three things must be unchanged, each a distinct defect:
  - The open count: a second `IHdmiCec::open()` on a held session fails with `EX_ILLEGAL_STATE` on
    a real, single-instance HAL, so a duplicate open reaching the HAL would raise `IOException` on
    a device while passing against this fake, which tolerates duplicates deliberately so the guard
    is testable at all — hence the assertion is on the counter, not the exception.
  - The controller: replacing it would invalidate the session the middleware has been
    transmitting on and strand the old one HAL-side with nothing able to close it.
  - The listener: replacing it would leave the HAL holding a listener whose owner moved on, and the
    fixture's close/reset/open cycle depends on re-registration not happening implicitly.

### DriverAidlSessionTest.ACallbackAfterAFailedCloseOrOwnerDestructionIsDroppedNotDelivered

- The hazard is a use-after-free (CWE-416). The event listener is a binder object the HAL holds a
  strong reference to, so its lifetime is not the middleware's to end, and it reaches its owner
  through a back pointer to a `DriverAidlImpl`. A listener still registered when its owner's
  storage goes away is a live object holding a dangling pointer, and the next callback — on the
  HAL's schedule, not the middleware's — dereferences it. The two ways in are the two arms driven:
  a failed close, where a detachment placed after the error check would be skipped, and a
  destructor.
- The guarded fix: detachment on both arms of `close()`, before the result is evaluated, and
  unconditionally in the destructor — not only when the state says a session is open, and not
  only when its own close succeeded.
- The drop is reported: the detached listener logs
  "DriverAidlImpl::EventListener: message received after detach, dropping it" and returns ok,
  the only visible trace a dropped oneway callback can leave. The line is matched verbatim so a
  changed message cannot keep passing; a non-ok return would reach `onTransact`.
- The run surviving is part of the assertion: a stale back pointer would corrupt memory or fault
  rather than fail, and the log assertion turns that into a diagnosis instead of a later crash.
- A local `DriverAidlImpl` is used so its destruction can be driven from the case; the
  process-global driver outlives every case. The retained `sp<>` keeps the listener alive past its
  owner, as the HAL's reference does on a device; without it the hazard would be unreachable and
  unasserted. At destruction the state is already CLOSED, so the destructor's own close is a
  silent no-op and the unconditional detachment is what runs.
- Requires invocation B; on a host with no binder driver these lines are compiled and type-checked
  but never executed, and must not be read as evidence there.
- The non-delivery check is the other half of "touched nothing": a dropped frame must not reach an
  application listener by another route.

### DriverAidlSessionTest.OpenRejectsANullControllerEvenWhenTheStatusIsOk

- Two conditions converge on one guard in `DriverAidlImpl::open()` —
  `if (!txn.isOk() || (controller == 0))` — and the null-controller arm is the easy one to omit,
  because a status of ok reads as success.
- Accepting a null controller would leave the state OPENED with no session, and every later
  transmit would fail on the controller check instead, far from the cause; so the state is asserted
  not to have moved, through a guarded `write()`.

### DriverAidlSessionTest.OpenRejectsANonOkBinderStatus

- The other arm of the same guard, asserted separately: one condition, two independent ways to
  satisfy it.
- The recovery is half the evidence, not a tidy-up. Two things must hold after a failed open, and
  neither follows from the `IOException` alone:
  - The state must not have moved. `open()` sets OPENED only after both halves of its guard pass,
    so a failed open leaves a CLOSED driver, observed through a guarded operation because the
    state is private. Setting the state first and raising afterwards would leave the driver
    believing it holds a session the HAL refused.
  - The failure must not be sticky. The recovery open must reach the HAL, asserted on the fake's
    open counter rather than on the absence of an exception: a driver wrongly kept OPENED would
    return silently from `open()` and look identical while nothing was re-established.
- The failing open is asserted to have reached the HAL, so the exception is the guard's and not
  something raised before the transaction.

### DriverAidlSessionTest.GetLogicalAddressReportsZeroOnTransportFailureWithoutRaising

- This arm keeps the AIDL back-end's failure signalling identical to the legacy back-end's.
  On either back-end `getLogicalAddress` reports a transport failure (a non-ok status) as 0 and
  never raises it: the legacy implementation ignores its HAL's return value and yields whatever
  the call left in a zero-initialised local.
- Raising would be a new exception on a path `LibCCEC::getLogicalAddress` does not guard — it would
  propagate unhandled into the Source plugin's discovery path — and would bypass the
  `InvalidStateException` LibCCEC raises for a zero.

### DriverAidlSessionTest.RemoveLogicalAddressIgnoresTransportFailureAndStillRemovesLocally

- A non-ok status on `removeLogicalAddresses` is ignored, matching the legacy disposition of
  discarding the HAL return; the local removal stands because it precedes the call and is not
  rolled back.
- This is the transport arm of the remove disposition; the refusal arm is asserted by an earlier
  case. They are separate branches of `DriverAidlImpl::removeLogicalAddress()`, and either one
  raising would diverge from the legacy back-end.

### DriverAidlSessionTest.TheFourUnconsumedAidlMethodsAreNeverCalled

- Each exclusion has a reason. `getState`: the middleware keeps its own CLOSED/CLOSING/OPENED state
  machine, and a second source of truth would be worse than none; `poll()` in particular is a CEC
  ping via a one-byte transmit, not a state query. `getProperty`: `HAL_CEC_VERSION` and the
  `METRIC_*` properties have no legacy counterpart, so reading them would be new behaviour.
  `registerEventListener`/`unregisterEventListener`: they exist for non-controlling diagnostic
  clients, whereas this back-end is the controlling client and receives events through the
  listener it passes to `open()`; calling them would register a second listener.
- Asserted rather than commented because an absence erodes silently. `getState` is the most
  tempting of the four (a cheap way to answer "is the HAL up"), and a change consulting it would
  fail here as a visible decision rather than become a quiet second state machine.
- The whole session lifecycle has run by this case (SetUp closed, reset and re-opened), and a
  representative add, write, poll and address query run first so the zeros cannot be explained by
  nothing having happened.

### DriverAidlTransmitTest

- Separated from `DriverAidlSessionTest`, with which it shares `DriverAidlSessionFixture`, because
  its subject is the transmit path (`write()`, `poll()` and `writeAsync()`) rather than session
  lifecycle, address marshalling and event delivery. The cases vary the reported send status, the
  destination, the frame length and the transaction status, and the fixture's helpers
  (`transmitWithStatus()`, `expectExactlyOneFrameOfLength()`) keep each case's body to its own
  claim.
- `expectExactlyOneFrameOfLength()` checks both the count and the size because "the call
  happened" is never the claim: a truncated or padded frame reaches the bus as a different CEC
  message.

### DriverAidlTransmitTest.DirectedFrameAcknowledgedByTheFollowerSucceeds

- `ACK_STATE_0` on a directed frame means the addressed follower acknowledged it. This is the arm
  where the inverted sense bites hardest, because the same value means "rejected" on a broadcast
  (asserted by the later broadcast cases); reading it as a rejection would fail every successful
  directed transmit.
- Success is asserted as "these exact bytes reached the HAL" (header `0x40`, opcode
  `GIVE_DEVICE_POWER_STATUS`), not as "no exception", so a transmit that dropped or rewrote the
  frame cannot pass.

### DriverAidlTransmitTest.DirectedFrameNotAcknowledgedRaisesNoAck

- Reproduces `DriverImpl.cpp:276-278`, where the legacy `SENT_BUT_NOT_ACKD` raises the same.
- The exception type is the substance: Connection and the plugins above it distinguish "no device
  answered" from "the transmit failed", and collapsing both into `IOException` would make an
  absent peer indistinguishable from a broken HAL.
- The frame is asserted to have reached the HAL, because the exception describes the bus outcome
  rather than a refusal to transmit.

### DriverAidlTransmitTest.BroadcastFrameNotRejectedSucceeds

- The single fact that makes the translation non-trivial: a broadcast has no addressed follower,
  so nothing acknowledges it, and `ACK_STATE_1` there means "sent and not rejected".
- An implementation mapping `ACK_STATE_1` to `CECNoAckException` unconditionally would compile,
  pass every directed case, and fail every broadcast the middleware sends — most of what the Sink
  emits during discovery.

### DriverAidlTransmitTest.RejectedBroadcastReturnsNormallyForOpcodesOutsideTheCtsArm

- `ACK_STATE_0` on a broadcast means some follower rejected it. For most opcodes that returns
  normally, matching the legacy implementation, which raises only on the CEC CTS 9-3-3 arm (a
  rejected `REPORT_PHYSICAL_ADDRESS`); raising here would make every rejected broadcast an error
  callers do not expect.
- This is the negative control for the CTS case: without it, an implementation that raised on
  every rejected broadcast would pass the CTS case and be wrong everywhere else.

### DriverAidlTransmitTest.RejectedBroadcastReportPhysicalAddressRaisesForTheCtsRetry

- The raise makes the caller retry at least once, which the CEC CTS 9-3-3 conformance test
  requires; without it the caller has nothing to retry on.
- This is the one opcode-specific arm in the whole translation (the broadcast branch of
  `DriverAidlImpl::write()`, mirroring `DriverImpl.cpp:279-284`). `REPORT_PHYSICAL_ADDRESS` is
  pinned by a `static_assert` at the top of the file because, were it to drift, this case and its
  negative control would silently test the same opcode and the distinction would vanish.

### DriverAidlTransmitTest.OneByteBroadcastDoesNotReachTheCtsArm

- The CTS guard is `(length > 1) && ((frame.at(1) & 0xFF) == REPORT_PHYSICAL_ADDRESS)`, and the
  length conjunct is not decoration: `DriverAidlImpl::poll()` transmits exactly such a one-byte
  frame, so reading `frame.at(1)` without the length check would raise `std::out_of_range` out of
  every poll — and poll is how the Sink discovers the bus.

### DriverAidlTransmitTest.BusyIsATransmitFailureOnBothDestinations

- `BUSY` means arbitration failed after two attempts and the message was not sent. That is the
  legacy send-failed family, hence `IOException`, on both destinations, because nothing reached
  the bus for the acknowledgement sense to apply to.
- Both destinations are asserted in one case because the answer must not depend on the
  destination: `BUSY` is checked ahead of the acknowledgement matrix in `DriverAidlImpl::write()`,
  before the nibble is read, and folding it into that matrix would give two answers for one
  failure.
- The broadcast half uses the CTS opcode, whose `ACK_STATE_0` arm raises `CECNoAckException`, so a
  different exception type there means `BUSY` fell through into the matrix.

### DriverAidlTransmitTest.NonOkBinderStatusOnSendRaisesIoException

- Reproduces `DriverImpl.cpp:261-263`, where a HAL error return raises the same.
- The transport check precedes the reported send status in `DriverAidlImpl::write()`. The
  out-parameter is meaningless when the transaction did not complete, so a translation consulting
  it first would classify a dead binder by whatever happened to be in that variable. The canned
  status is `ACK_STATE_0`, whose own arm succeeds on a directed frame, so a pass proves the
  transport check ran first.

### DriverAidlTransmitTest.FramesOverTheAidlLimitAreRefusedWithoutBeingSentOrTruncated

- The AIDL half of the frame-size difference between the back-ends. 16 bytes
  (`kAidlMaxMessageLength`, the `sendMessage` contract's stated maximum) is accepted, matching the
  legacy back-end, which confines the difference to the disputed 17-to-20 band rather than to the
  guard's existence. 17 (`kJustOverAidlLimit`) and 20 (`kLegacyMaxMessageLength`) are refused with
  `IOException`.
- The nothing-was-sent assertion is the point. An `IOException` raised after a truncated frame
  reached the bus would be strictly worse than either outcome alone: the caller sees a failure
  while a corrupt CEC frame is already on the wire, and a peer may interpret it as a different
  message. So the send counter is asserted to be zero and the captured message empty, not merely
  the exception raised; the guard runs ahead of the transmit in `DriverAidlImpl::write()`.

### DriverAidlTransmitTest.WriteAsyncRaisesOperationNotSupportedOnAnOpenDriver

- The AIDL half of the `writeAsync` difference: `OperationNotSupportedException` on an open
  driver, raised after the prelude and the state guard have both passed. The prelude ordering and
  the state guard are identical on both back-ends, so a different outcome here either emulates
  asynchrony or has lost the guard. This is the one arm that needs an open driver, which is why it
  is not with the prelude cases.
- Asynchronous transmit is not migrated and deliberately not emulated — no threads, no work queue,
  no deferred callback, no wrapper — which is safe because no production call site reaches it:
  every plugin transmit goes through `Connection::sendToAsync` and `Connection::sendAsync` onto
  the Bus writer thread, which then calls the synchronous `write()`.
- Nothing may be transmitted, asserted rather than assumed: an implementation that forwarded the
  frame to `sendMessage` before raising would put it on the bus while telling the caller the
  operation is unsupported.

### DriverAidlTransmitTest.PollTransmitsAOneByteFrameCarryingBothAddresses

- The byte layout is asserted, not just the call: initiator in the high nibble and destination in
  the low one, composed in `DriverAidlImpl::poll()`, so a ping a peer would see as addressed to
  the wrong device cannot pass.
- `getState()` is deliberately not used for a poll: a poll is a transmit that is either
  acknowledged or not, and answering it from the HAL's state would answer a different question —
  which is why `DriverAidlSessionTest.TheFourUnconsumedAidlMethodsAreNeverCalled` checks the state
  counter stays at zero.

### DriverAidlTransmitTest.UnansweredPollRaisesNoAck

- A poll is a directed one-byte frame, so `ACK_STATE_1` means not acknowledged and the directed
  arm applies; `CECNoAckException` is how "no device there" is reported all the way up to Bus.
- Bus discovery depends on this arm: a poll reporting success for an absent device would populate
  the device list with devices that are not there.

## tests/L2Tests/test_main.cpp (part 1 of 2)

Detail moved out of the condensed comments in the first part of the L2 runner's entry point
(file header through the `ProcessGroupState` enum). Nothing here changes the code's behaviour.

### File header (`@file test_main.cpp`)

- The unit owns everything the L2 cases depend on and nothing they may touch directly: the legacy
  HAL double, the out-of-process fake service host, every descriptor and child process in the
  binary, and the bring-up order. The child half of the lifecycle is
  `mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp`.
- **Why a second process.** libbinder resolves a service registered in the calling process to the
  local `BBinder`, so `interface_cast` returns that object: no `Bp*` proxy, no driver transaction,
  no client threadpool. An in-process fake (the L1 tier's in-process modes) therefore cannot
  exercise the transport. Hosting the fake in another process makes the middleware hold a real
  proxy and receive callbacks on a binder threadpool thread. The build enforces the split: the
  runner compiles the legacy mock, the host binary compiles the fake, and neither carries the
  other's source.
- **Every setup fault fails the run.** Mode handling, host launch, readiness wait, channel
  handshake and `LibCCEC::init()` each raise a fatal GoogleTest failure on their unhappy paths.
  `init()` is asserted rather than wrapped: what it raises is real (`Driver::getInstance().open()`
  refused by the selected HAL, or `Bus::start()` failing), and a failed initialization reported
  green would make every driver-dependent result meaningless.
- **Host lifecycle.** On `remote` the harness launches the host with an inherited readiness pipe,
  blocks on it under a bound and fails the run when the bound expires. At suite end `TearDown()`
  asks the host to shut down over the control channel, signals it, waits within a bound, escalates
  to `SIGKILL` and reaps it. Nothing the run starts may outlive it, because an orphaned host holds
  the production service name and makes the next run fail on a stale registration.
- **Readiness is a token on a pipe, not a timed wait.** A sleep converts a race into a flake (too
  short when loaded or emulated, wasteful otherwise) and never proves publication. The host writes
  no readiness line on any startup failure (its serving and shutdown-wait failures come after the
  line), so this wait is the only detector of a host that failed to register, could not reach a
  binder transport, or is blocked in libbinder waiting for handle 0 because no service manager
  runs. Timeout, end of file without the token, and a mismatched token all fail the run.
- **Control and observation channel.** `remote` hands the host two more inherited descriptors,
  named in `CEC_FAKE_HOST_CONTROL_FD` (read end of a command pipe this harness writes) and
  `CEC_FAKE_HOST_OBSERVE_FD` (write end of a reply pipe this harness reads, one line per command).
  The host's file block is the normative protocol statement; this file is the client half. Without
  it an outbound transmit is visible only as "sendTo did not throw" (a dropped or corrupted send
  satisfies that), and an inbound delivery cannot be caused at all, since only the fake service in
  the host can invoke the listener.
- **`CEC_TEST_AIDL_MODE`.** Read and acted on only here and in `tests/L1Tests/test_main.cpp`.
  `tests/L2Tests/ccec/test_DualPathIntegration.cpp` never reads it: it asks this file through the
  seam `cecL2RequestedAidlMode()`, only to assert that the resolved back-end matches the requested
  mode. No production source reads it.
  - `absent`, which an unset or empty variable also means: launch nothing; the legacy back-end is
    selected and driven through the in-process mock. Invocation D.
  - `remote`: launch the host with readiness pipe and channel, wait for the token, ping the
    channel once, then initialize. The middleware resolves a real proxy and its listener callback
    arrives on a binder thread. Invocation E.
  - `compatible`, `incompatible`: in-process modes owned by `run_L1Tests`; an in-process
    registration yields no proxy, no driver transaction and no binder-thread callback, so either
    value is a fatal failure naming that runner rather than a downgrade to `absent`.
  - Any other value is fatal: every value besides `absent` and `remote` fails the run. Unset and
    empty are permissive only because both mean `absent`, the tier's default, so a bare
    `./run_L2Tests` legitimately runs the legacy arm; a typo silently downgraded would report a
    green result for an invocation that never happened.
- **Threadpool.** This file does not start the binder client threadpool: `DriverAidlImpl::open()`
  owns that inside `init()`. The host starts its own service-side pool, a different pool serving
  the other direction.
- **Stale registration.** A service already published under the production name is fatal, and the
  check is the host's: it queries the name with `checkService()` and exits with its own code
  without a readiness line, which the bounded wait reports. Checking here would mean this process
  reaching the service manager, which on the pinned binder stack aborts the process when no driver
  node exists and blocks indefinitely when no service manager runs; neither may be risked in a
  runner that also executes the legacy invocation on a host without binder. On `absent`, where no
  host runs, it is detected from the resolved selection instead: after `init()`,
  `failUnlessSelectedBackEndMatchesMode()` fails the run when the factory did not select the
  legacy back-end, by `dynamic_cast` and with no binder call.
- This translation unit includes no binder or AIDL header and makes no direct binder API call.
  On the legacy (`absent`) invocation it launches no host and hosts no fake; only the `remote`
  invocation launches the separate host binary, `fake_hdmi_cec_aidl_host`, named by
  `CEC_FAKE_AIDL_HOST_PATH`. The runner itself links libRCEC and the AIDL/binder libraries, and
  `init()` runs the production back-end selection on every invocation: without a binder driver
  node its preflight declines before libbinder is reached, while with a node and a service manager
  even the legacy invocation makes a libbinder `checkService()` lookup, which finds no registered
  HDMI CEC service.
- **SIGPIPE** is ignored for the environment's lifetime and the entry disposition restored
  afterwards, so a write to a pipe whose reader has gone reports `EPIPE` to the requesting case
  instead of killing the process before teardown reaps the host. Observations travel over a pipe,
  not binder, because evidence carried over binder would attest to the transport with the
  transport. See `ignoreBrokenPipeSignal()`.
- **Ordering.** `init()` is the first call that forces `Driver::getInstance()`, which constructs
  both back-ends, asks the AIDL one whether its service came up, emits one selected-path line and
  fixes the choice for the process. Launching the host after `init()` leaves the selection on
  legacy: every case passes and the run is reported as AIDL evidence without ever speaking binder.

### POSIX include group

- There is no binder or AIDL header among the includes and no direct binder API call in this
  translation unit: the harness launches a binary and reads a pipe, and must not make binder calls
  itself (see "Stale registration"). The runner as a whole is not binder-free: it links libRCEC and
  the AIDL/binder libraries, which the production selection path uses.

### `g_hostPid`

- Module scope because `SetUp` acquires the host and `TearDown` releases it. The `-1` sentinel lets
  teardown after a partial setup (the legacy invocation, or a launch that failed halfway) see that
  there is nothing to signal or reap. GoogleTest runs `TearDown` even after a fatal `SetUp`
  failure (measured behaviour), so the distinction is exercised on every failing run.

### `g_hostProcessGroupId`

- Holds the same number as `g_hostPid` (the child makes itself a group leader, so its group id is
  its pid) but a different fact: `g_hostPid` means "one child to reap"; this means "its descendants
  are collected under a group id this harness owns, so a signal to its negation reaches all of them
  and nothing else".
- It stays `-1` until confirmed because every process starts in its parent's group: before the
  child's `setpgid()` its group is the runner's, and `kill()` to the negation of an unconfirmed
  number would signal the runner and every sibling in its group. It is set only after `getpgid()`
  shows the child's group equals its pid and differs from the runner's group.

### `g_hostReadinessReadFd`

- Held open for the host's lifetime rather than closed once the token arrives, because end of file
  on it occurs when and only when the host process exits and closes its write end. That makes it a
  death channel and lets `TearDown` wait for the host without polling a timer.

### `g_hostControlWriteFd`, `g_hostObserveReadFd`

- Two requirements cannot be discharged inside the runner alone:
  - **Outbound.** A transmit that crosses the driver is observed only as "sendTo returned without
    throwing"; a corrupted or dropped send returns the same way. The bytes received live in the
    host, and the fake is deliberately not linked into the runner (that would make the tier
    in-process again), so the only way to read them is to ask the host.
  - **Inbound.** Only the fake service in the host can invoke the middleware's
    `IHdmiCecEventListener`, so without a request to fire a callback no case can cause an inbound
    delivery or assert that it arrives off the calling thread.
- The channel is an ordinary pair of inherited pipes, working identically whether the driver is
  healthy, degraded or absent; evidence over binder could be corrupted invisibly by the very fault
  under test.
- Module scope because `SetUp` acquires them, `TearDown` releases them, and the client function is
  called from the other translation unit in the binary.

### `g_hostReportedReady`

- Exists because the readiness-failure path calls the same teardown: a host that never became
  ready is dead or wedged, so a `shutdown` request would spend its whole bound learning what the
  signal establishes immediately.

### `g_previousSigpipeAction`, `g_sigpipeDispositionReplaced`

- Module scope because `SetUp` installs the replacement and `TearDown` restores the original. The
  flag prevents a `TearDown` that follows a `SetUp` failing before its first step from installing a
  default-constructed disposition over the process's real one.

### Untrusted-value rendering contract (`renderUntrustedValue`, `RENDER_LIMIT`)

- **Defect removed (CWE-117).** Diagnostics once streamed caller-supplied text verbatim, so a value
  containing a newline ended the message and started a line of its own; a line starting
  `::error::` is a GitHub Actions workflow command. Measured before the fix:
  `CEC_TEST_AIDL_MODE=$'bogus\n::error::FORGED_L1_ANNOTATION'` produced a standalone forged
  annotation, and a 5000-character value produced over 10 KB of diagnostics burying the real
  failure.
- **Contract, in application order:**
  1. A literal backslash becomes `\\` first, so every later escape is distinguishable from the same
     characters occurring literally.
  2. `0x0A` → `\n`, `0x0D` → `\r`, `0x09` → `\t`; every other byte outside printable ASCII
     `0x20..0x7E` → `\xNN` in lower-case hex. Classification is by unsigned byte value with no
     locale-sensitive call (`isprint`, `iswprint`, ctype), so it behaves as under `LC_ALL=C` and no
     multi-byte sequence can hide a control character.
  3. Output is truncated at `RENDER_LIMIT` characters, appending `...[truncated, N bytes total]`
     with N the value's own byte length.
  4. It applies to the value, never the surrounding message, and the result is always delimited
     by `"`, so an empty value shows as `""`.
  5. It never begins a diagnostic line: every message keeps its own prefix and the rendering starts
     with `"`. With rule 2 leaving no raw newline, a forged standalone `::error::`, `::warning::` or
     `::notice::` line is unreachable by construction.
- **Postcondition.** The result contains no byte below `0x20` or above `0x7E`, so it cannot end the
  line it sits on or begin a new one. Rendering a whole message would escape its punctuation and
  destroy the prefix rule 5 relies on.
- **Five copies of one contract,** which must not diverge:
  `hdmicec/tests/L1Tests/run_coverage.sh` (`render_untrusted()`),
  `hdmicec/.github/workflows/aidl-path-tests-rootfs.sh` (`render_untrusted()`),
  `hdmicec/tests/L1Tests/test_main.cpp`, `hdmicec/tests/L2Tests/test_main.cpp` and
  `hdmicec/mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp` (`renderUntrustedValue()`). They are
  file-local rather than shared because two are shell and three C++, across three build targets
  and one non-built script, and a shared header would add a build edge for a twenty-line function.
- The `std::string` and C-string overloads are `inline` so a copy whose file does not call one of
  them raises no `-Wunused-function` warning.
- The value accepts any content: embedded newlines, carriage returns, ANSI escapes, invalid UTF-8,
  multi-kilobyte payloads. The result is never longer than `RENDER_LIMIT` plus the truncation note
  and two quotes.
- The C-string overload renders null as the undelimited `<unset>` because "unset" and "set to the
  empty string" are different facts.

### Removed helper `renderForDiagnostic()`

- It once sat after `startFakeServiceHost()` and was deleted rather than kept beside
  `renderUntrustedValue()`. It dropped only a *trailing* newline and truncated at 120 characters, so
  a newline elsewhere in bytes from the host process still ended the diagnostic. All ten call sites
  now use `renderUntrustedValue()`, and the manual `"..."` wrapping each added is gone because that
  helper delimits its own output. One renderer avoids the divergence the five-copy contract exists
  to prevent.

### `AIDL_MODE_*` constants

- The spellings are a fixed contract shared with `tests/L1Tests/test_main.cpp`, `run_coverage.sh`,
  both CI workflows and the test documentation; they are not free to change here.

### `HOST_PATH_VARIABLE`

- The host itself never reads it: it is already running by the time it would matter.

### `HOST_READY_FD_VARIABLE`

- Carries the number of the write end of the readiness pipe: this harness owns the pipe and names
  the descriptor. The host parses strictly and refuses anything but an unadorned non-negative
  decimal, so it is formatted with no sign, padding or surrounding space.

### `HOST_READINESS_TOKEN`

- Copied from `mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp`, not invented. The host writes it
  once, with a raw unbuffered write, only after publication has succeeded and its service-side
  threadpool is running.
- The newline is part of the token because a line reader needs the terminator to know the line is
  whole; a partial write delivering the name alone must not count as readiness. Everything else the
  host prints goes to standard output, so the pipe carries the token and nothing else.

### `HOST_READINESS_TIMEOUT_MS`

- The bound covers a cold start of a second process that opens the binder driver, reaches the
  service manager (retrying at one-second granularity until handle 0 resolves, so a few hundred
  milliseconds could expire inside one healthy retry), publishes a service and starts a threadpool.
- The AIDL invocation runs in an emulated guest on a loaded CI machine, slower by an unpredictable
  margin. Thirty seconds is far above any healthy startup and far below any CI job timeout, so a
  genuine failure is reported with a diagnostic in under a minute instead of hanging until killed
  from outside.

### `HOST_SHUTDOWN_TIMEOUT_MS`

- The host's shutdown path is a self-pipe write, a few closes and a return. A host still present
  after five seconds is most plausibly blocked inside libbinder; waiting longer only trades a
  diagnosable failure for a hung suite. Escalation keeps a wedged host from outliving the run,
  holding the production service name and failing the next run on a stale registration.

### `HOST_READINESS_MAX_BYTES`

- Bounding accumulation keeps a misdirected stream from growing a buffer without limit while the
  timeout has not yet expired. Reaching the cap with no newline ends the wait as `TokenMismatch`.

### `HOST_CONTROL_FD_VARIABLE`, `HOST_OBSERVE_FD_VARIABLE`

- A fixed contract owned by the host source, whose file block is the normative protocol statement.
  The host exits with its own code and no readiness token if one is set without the other, if
  either is not a plain non-negative number, if either names a descriptor not open in the child or
  open in the wrong direction, or if both name the same descriptor. The harness sets both or
  neither and never derives one from the other.
- Control carries the descriptor the host reads commands from (read end of a pipe this harness
  writes); observe carries the descriptor the host writes replies to (write end of a pipe this
  harness reads). Swapping them is refused by the host rather than half-working.

### `HOST_CONTROL_REPLY_TIMEOUT_MS`

- Every command is answered from memory by a host already serving: observation commands read a
  counter, and trigger commands invoke a `oneway` callback that returns without waiting for the
  middleware. A healthy reply takes well under a millisecond.
- The bound exists for a host that stopped serving its loop and a host that died between the
  readiness token and the command; both must present as a failed assertion naming the command,
  never as a hung suite killed from outside with nothing recorded.
- Generous on purpose: a bound tuned to a developer host would turn emulated-guest CI load into a
  flake.

### `HOST_CONTROL_MAX_REPLY_BYTES`

- The longest reply is `calls`, sixteen counters in well under a kilobyte. Bounding the
  accumulation stops a misdirected descriptor from growing a buffer without limit inside an
  unexpired wait; same disposition and value as `HOST_READINESS_MAX_BYTES`.

### `CHILD_EXIT_PRE_EXEC_FAILED`, `CHILD_EXIT_EXEC_FAILED`

- They separate "the host ran and refused" from "the host was never reached" without consulting
  either file. The host's codes are file-local to its translation unit and deliberately not
  duplicated: this harness reports the raw status, and the host's trace on standard output names
  the step it reached.

### `ReadinessOutcome`

- A timeout means the host is still running and has not published; end of file means it exited
  before publishing; a mismatch, whether a complete non-token line or `HOST_READINESS_MAX_BYTES`
  with no newline, means something other than the host is on that descriptor. A CI log reader has
  only this to go on, so the cases are not collapsed into one "not ready".

### `writeRawFully()`

- The function calls nothing that allocates or locks, because every caller is a forked child where
  doing either is unsafe: the host-launch child writes its pre-exec and failed-`execve()`
  diagnostics with it, and each probe child its one-byte handshake. A diagnostic is the only way the
  launch child's failure can say anything. Every buffer it is given is a string literal with a
  compile-time length, so not even `strlen()` is involved.
- A failed write is discarded because reporting it would breach the no-allocation constraint, and
  each caller surfaces the outcome another way. The launch child next calls `_exit()`, and its exit
  status carries the outcome. A probe child's failed handshake leaves its parent's bounded wait
  without the byte, so that step fails, by timeout or by end of file if the child dies first.

### `DESCRIPTOR_SWEEP_FALLBACK_CAP`

- `close_range()` closes a whole span in one syscall and needs no bound. The `close()` fallback
  cannot use the descriptor limit unmodified: `RLIMIT_NOFILE` measured 1048576 on the development
  host, so an uncapped loop would issue a million syscalls between `fork()` and `execve()`.
- The cost of the cap is that, without `close_range()`, a descriptor above it would survive into
  the host. This project does not take that path: glibc provides `close_range()` since 2.34 and the
  kernel since 5.9, both already required by other means.

### `closeDescriptorRange()`

- Post-fork safety matters because the process may have a binder threadpool running: a fork copies
  each lock's state as it stood, and a child taking one could block on a lock no thread of its own
  will release. The function uses only `close_range()`, `getrlimit()` and `close()`.
- Errors are ignored because closing a never-open descriptor fails with `EBADF`, the expected
  result for most of the span, and each forked caller proceeds regardless: the host-launch child
  to `execve()`, the group-probe child to `fork()`.
- `close_range()` fails with `ENOSYS` on kernels older than 5.9, in which case the loop runs.

### `closeInheritedDescriptorsExcept()`

- A fork duplicates every descriptor, and the runner holds more than the four it created for the
  host: GoogleTest's output file when requested, the binder driver node on an invocation that
  resolved the AIDL back-end, and whatever earlier probes left open. Each inherited descriptor is
  held by the host and anything it starts for their lifetimes; a descendant holding a pipe's write
  end keeps that pipe from ever reporting end of file, so the death channel never fires.
- The three kept descriptors are parameters because the host is told those numbers in its
  environment and depends on them surviving the exec, which is why the caller clears `FD_CLOEXEC`
  on exactly those three immediately before. Standard streams are kept because the host writes its
  trace there and the runner's log must receive it; the sweep starts at `STDERR_FILENO + 1`.
- Called in the parent it would close the descriptors the parent uses to talk to the host.
- Postcondition: no descriptor above `STDERR_FILENO` other than the named ones is open, so only
  those reach the host.
- The sort is a fixed three-element bubble over locals: no allocation, comparator object or library
  call after a fork. Sorting lets the sweep be expressed as the gaps between kept numbers, which is
  what makes `close_range()` usable.

### `describeWaitStatus()`

- The exit status is the load-bearing half of a host failure diagnostic because the host gives each
  failure class its own code, and a startup failure writes no readiness line. It is not translated
  into the host's vocabulary: those codes are file-local to the host, and a second copy would be one
  more thing to keep in step.

### `reapChildBlocking()`

- The child is known to be leaving, either because end of file on a descriptor it held shows it
  closed its descriptors, or because it was sent `SIGKILL`, which cannot be caught, blocked or
  ignored. Reaping is mandatory: an unreaped child becomes a zombie that outlives the suite.
- This is the bounded end of an unbounded primitive: `terminateAndReapChildProcess()` reaches it
  only after an observed exit or its own `SIGKILL`, and `EpipeProbeResources`' destructor sends
  `SIGKILL` immediately before calling it. It is never reached on a `SIGTERM` alone, because a child
  may handle `SIGTERM` and one of this harness's children does.

### SIGPIPE disposition (`ignoreBrokenPipeSignal()`, `restoreBrokenPipeSignalDisposition()`)

- **Hazard.** SIGPIPE's default disposition terminates the process, and exactly one write here can
  provoke it: `writeControlCommand()` writing to the host's control descriptor, whose reader can exit
  or close at any moment. Under the default the write never returns, and two contracts are lost:
  - The diagnostic: `writeControlCommand()`'s `EPIPE` arm, which names the host as gone in the
    requesting case's failure, never runs; a signal delivers no errno, GoogleTest records no
    result, and the run reads as a crash of unknown origin through the one path that bypasses the
    channel's bound.
  - The reap: a signalled process runs no teardown, so a wedged or still-serving host survives the
    run holding the production service name and fails the next run on a stale registration.
- **With the disposition installed** the write returns -1 with `EPIPE`, `writeControlCommand()`
  fails with a sentence naming the command, `performHostControlRequest()` returns false, the case's
  assertion fails at its own line, and `TearDown` reaps the host and reports how it ended.
- **Proved by a test.** `cecL2ProveEpipeDiagnosticAndChildReaping()` builds the hazard from a pipe
  and a child of its own, calls the real `writeControlCommand()` on a descriptor whose reader has
  gone, requires the false return and the `EPIPE` sentence, and ends that child through the same
  `terminateAndReapChildProcess()` teardown uses. Behaviour without the disposition cannot be checked
  (it would terminate the runner), so the case first reads the disposition back and fails fatally
  if it is the default.
- **Scope: the whole environment,** installed as the first step of `SetUp` and undone as the last
  step of `TearDown`, rather than around each write. The seam case runs from a fixture that never
  skips, so it also runs on the legacy invocation, where no host is launched and a disposition
  installed beside the pipes would leave it unprotected. The first statement of `SetUp` is also a
  place no later reordering can skip.
- **The host chooses differently, without contradiction:** it installs the disposition only where a
  channel was supplied, because it runs no test of its own and leaving process defaults alone
  minimises the difference between runs with and without a channel.
- **No effect on the code under test.** CCEC's transports are an in-process C function-pointer ABI
  (legacy) and binder ioctls (AIDL); neither writes to a pipe or socket, and no `ccec/` or `osal/`
  source installs, blocks, raises or waits on SIGPIPE.
- **Every other descriptor operation was checked:** the readiness pipe and observation pipe are read
  ends only (reads cannot raise SIGPIPE); `writeRawFully()` writes to `STDERR_FILENO` in the
  pre-exec child and to a handshake pipe in each probe child, all of which inherit this
  disposition; `std::cout` writes to standard output, inside the protected window.
- **Why the original is restored.** A disposition is process-wide and outlives the environment;
  anything after global teardown (another environment's `TearDown`, static destructors, `atexit`
  handlers) would otherwise inherit an unannounced choice.
- `SIG_IGN` survives `fork()` and `execve()`, so the host starts with SIGPIPE ignored; it installs
  its own disposition regardless, since a program that must not die on a closed pipe cannot depend
  on how it was launched.
- `ignoreBrokenPipeSignal()`: continuing after a false return would run a harness whose
  control-channel write can terminate it and skip the host reap; it is a setup fault, not a degraded
  mode.
- `restoreBrokenPipeSignalDisposition()`: on false, SIGPIPE remains ignored process-wide.

### `startFakeServiceHost()`

- **On success** `g_hostPid` and `g_hostReadinessReadFd` (and the channel descriptors) are set, and
  the caller must wait for the readiness token before anything resolves the back-end selection.
  `g_hostProcessGroupId` is set only where it could be confirmed. On failure every error path closes
  what it opened, so a failed launch cannot leak a descriptor into a run reporting a different
  failure. On success a child exists and must be reaped whether or not it reports ready.
- **argv and environment are built before the fork** so the child's path to `execve()` neither
  allocates nor locks. `setenv()` in the child would be shorter but may reallocate the environment
  block; copying the block and appending entries removes the question.
- **Descriptor inheritance.** All pipes are created with `O_CLOEXEC`; the child clears `FD_CLOEXEC`
  only on the three ends the host is told about (readiness write end, control read end, observation
  write end), so of the readiness pipe only the write end, not the read end, survives into
  the host image. The parent closes its copy of the write end right after the fork, so end of file
  means "the host closed its write end" rather than "the parent still holds one"; getting that close
  wrong turns a dead host into a hang.
- **Everything else is swept.** Clearing `FD_CLOEXEC` on three descriptors says what the host may
  have, not what the fork already gave it, so the child closes every other descriptor above the
  standard streams immediately before `exec`.
- **Process group.** The child calls `setpgid()` first and the parent repeats it and reads the group
  back with `getpgid()`. `waitpid()` collects one process; only a group signal reaches a process the
  host started. The read-back matters because an unconfirmed group id is the runner's own.
- **Pre-fork `access()` check.** It makes the common misconfiguration (empty, misspelled or unbuilt
  path) produce a sentence naming the path rather than an exec failure visible only as an exit
  status. It cannot be conclusive (the file can change between the check and the exec), so the
  child still handles its own exec failure.
- **Atomic `O_CLOEXEC` via `pipe2()`.** `pipe()` plus two `fcntl()` calls leaves a window in which a
  program another thread concurrently forks and execs inherits the descriptors (a fork alone always
  copies descriptors, which is why the child sweeps); the harness is single-threaded here today, but
  that is a property of the call site, not of the function.
- **Two channel pipes.** One descriptor cannot serve both directions: the host refuses both
  variables naming the same number, since it would read its own replies. So two pipes and four
  descriptors, of which each process keeps two.
- **Child environment.** Inherited values of the three descriptor variables are dropped because a
  stale number from an outer harness would be ambiguous and the host refuses to guess. All three
  are set together because the host treats one channel variable without the other as a hard
  failure. Pointers are taken only after all strings are appended because growing the vector moves
  the strings, and short strings keep their characters inside the object.
- **Child: closing the parent's channel ends.** If the child kept the control write end open, the
  host would never see end of file when the parent closes its copy, so a parent that vanished would
  leave the host serving a channel nobody drives.
- **Child: `setpgid(0, 0)`.** Without it the host runs in the runner's group, teardown can signal
  only the forked pid, and a host that forks (or a host path that is a shell script) leaves children
  holding copies of this harness's pipes, including the readiness write end whose closure teardown
  reads as "host gone". The group-wide signal would have killed the runner. Both sides call
  `setpgid()` (the standard idiom): one alone leaves a window in which the child execs and is
  signalled in the runner's group, or the parent reaches teardown before the child ran. A failure
  refuses the launch rather than silently falling back to single-pid signalling. `setpgid()` is
  async-signal-safe and allocates nothing.
- **Child: clearing `FD_CLOEXEC` on the channel ends.** Easily and silently forgotten: without it
  the host finds the numbers in its environment closed, refuses to start with its own code and
  writes no token. Done only for the descriptors the child keeps.
- **Child: sweep placement.** The sweep runs after the three `F_SETFD` calls so that its keep-set and
  the set whose `FD_CLOEXEC` was cleared (the same three descriptors) stay adjacent, making an
  edit to one obviously an edit to the other.
- **Child: after `execve()`.** Reached only on failure; the parent sees end of file plus the exit
  status, which its diagnostic distinguishes from a host that ran and refused.
- **Parent `setpgid()`.** Issued first so the window in which the child could be signalled in the
  runner's group is as short as possible. `EACCES` means the child already exec'd (so its own call
  succeeded); `ESRCH` means it already exited. Anything else is traced because the group would be in
  a state neither side established.
- **Group read-back.** A group is signalled only if its id equals the child's pid (so it holds the
  child and its descendants) and differs from the runner's group. If either fails, including
  `getpgid()` failing with `ESRCH` for an already-collected child, the id stays `-1` and teardown
  signals one pid.
- **Parent closes the child's channel ends** for the same reason in both directions: holding the
  control read end would hide end of file from the host, and holding the observation write end
  would stop a dead host producing end of file, so a reply wait would sit out its whole timeout.

### `awaitHostReadiness()`

- There is no sleep or polling interval: `poll()` gets the time remaining and returns as a byte
  arrives. The deadline comes from `steady_clock` rather than accumulated per-iteration timeouts, so
  repeated interruptions cannot extend it, and a wall-clock adjustment cannot shorten or lengthen it.
- `observed` receives the complete first line on a match or a non-token line, every byte received
  when `HOST_READINESS_MAX_BYTES` or more arrive without a newline (also `TokenMismatch`), the
  partial bytes on a timeout or end of file, or the failing call's errno text.
- On `ClosedWithoutToken` the host's exit status says why it exited.
- Continuing after anything but `Ready` would leave the selection on legacy and describe the result
  as AIDL.
- `POLLHUP` is reported whether requested or not and can arrive with buffered data, so pending bytes
  are consumed first and end of file is concluded only from a zero-length read.
- A partial read delivering the token's name without its newline is not readiness; accepting it
  would treat a truncated write as a published service.

### `controlReplyResidual`

- `read()` takes whatever is available, not one line, so a host that wrote two replies before either
  was read would otherwise lose the second, and every later request would get the previous reply.
  Keeping the surplus makes framing independent of how bytes are split, as in the host's own reader.
- A healthy session never fills it: the protocol is one reply per command and the client sends the
  next command only after reading the previous reply.

### `controlRequestMutex`

- GoogleTest runs cases sequentially and no case starts its own thread. If that changed, the
  consequence would be two commands interleaved on the wire, each reading the other's reply, which
  would be blamed on the transport under test. One uncontended lock per request costs nothing.

### `writeControlCommand()`

- A host that has stopped reading would fill the pipe buffer and block the runner indefinitely;
  polling for writability against the remaining time turns that into a reported failure.
- **Why the descriptor is a parameter.** The `EPIPE` arm exists only because SIGPIPE is ignored, and
  the broken-pipe case in `tests/L2Tests/ccec/test_DualPathIntegration.cpp`, through
  `cecL2ProveEpipeDiagnosticAndChildReaping()`, must call this very function on a descriptor whose
  reader has gone. Breaking the live channel (whose reader is the host) would destroy the session the
  rest of the invocation needs. The parameter is the only change; deadline, poll, `EINTR` handling,
  `POLLERR` fall-through and failure sentences are untouched, since a seam exercising a modified
  copy would prove nothing.
- `POLLERR` means the host closed its read end, which `write()` reports as `EPIPE`; it is not
  handled separately so that "the host has gone" has one diagnostic. Under SIGPIPE's default
  disposition this `write()` would not return: the process would die, the `EPIPE` arm and teardown
  would never run. If the install were removed, the arm would become dead code and the fall-through
  a way to die silently.

### `readControlReply()`

- End of file before a terminator (host exited) and expiry of the bound (host alive, not answering)
  are distinct outcomes because a CI log reader has only this to tell them apart.
- Trailing `\r` stripping is defensive: the host writes bare newlines.

### `performHostControlRequest()`

- Treating `ERR` replies as failures would deny a test the ability to assert on an expected refusal.
- **Pre-write validation.** An embedded newline would send two commands and read the first of two
  replies, leaving every later request one reply behind (presenting as unrelated later failures).
  An empty or whitespace-only line gets no reply by the host's contract, so the bounded wait would
  expire on an accepted command. Both are caller mistakes and are reported as such.
- A reply beginning with neither `OK ` nor `ERR ` is unclassifiable; accepting it would let a
  desynchronised channel pass as a well-formed refusal.

### `closeHostControlChannel()`

- A run that created the pipes and then failed to fork holds descriptors with no host behind them;
  they are released the same way. Closing the control write end is how a still-serving host learns
  the session is over (it reads end of file and shuts down cleanly), so this is part of the
  teardown contract, not only descriptor hygiene.

### `waitForChildExitViaPipeEof()`

- While the child lives it holds the pipe's write end, so end of file occurs when and only when it
  has gone: a real wait on the real event. A `WNOHANG` poll around a sleep asks repeatedly, notices a
  prompt exit up to one interval late and does work while idle; both are bounded by `timeoutMs`.
- The descriptor is a parameter so one implementation serves every child: the host's death channel
  is its readiness pipe, and the broken-pipe seam's is the handshake pipe it creates for its child.
- A child without a death channel is waited for by `waitForChildExitByPolling()`, so the bound
  belongs to `terminateAndReapChildProcess()` rather than to what its caller had available.
- Bytes arriving instead of end of file are not part of any contract and are drained.

### `CHILD_EXIT_POLL_INTERVAL_MS`

- The interval is the worst-case lateness in noticing an exit. Against the grace periods used (five
  seconds for the host, two for the broken-pipe probe child) it is invisible, and an ordinary
  termination costs a handful of wake-ups. Much smaller would spin a core; much larger would make
  prompt exits look slow and could delay `SIGKILL` past the requested grace period.
- It is not an alternative to the end-of-file channel; it governs only the fallback, where the
  choice is between polling and not bounding the wait.

### `PolledChildExit`

- A collected child was already reaped by the poll and must not be waited for again; a running child
  must be escalated to `SIGKILL`; a refused wait means the pid cannot be collected at all, a reported
  failure that a stronger signal would not fix.

### `waitForChildExitByPolling()`

- Without it, a child with no death channel would get an unbounded blocking reap, and for a child
  that handles, blocks or ignores `SIGTERM` (the fake host installs a handler) that wait has no bound.
- It is the second choice: end of file is the real event, seen at once; polling notices an exit up
  to one `CHILD_EXIT_POLL_INTERVAL_MS` late. Both share the caller's `timeoutMs`.
- Reaping here is forced by the mechanism: `WNOHANG` cannot see an exit without collecting it. A
  caller that waited again on an already-collected pid would get `ECHILD` and misreport a collected
  child as lost.
- Parameters: the caller refuses a non-positive pid before signalling; a non-positive `timeoutMs`
  performs exactly one non-blocking check, the honest reading of a zero grace period; `description`
  is untouched on `StillRunning`, where the caller's escalation and blocking reap produce it.
- Postcondition on `Collected`: no child exists for the pid and nothing more may be waited on it.
- `EINTR` is retried, not reported: giving up would return `StillRunning` for a child that had exited
  and escalate to `SIGKILL` needlessly.
- The positive-result test is on the sign, not equality with `childPid`, because `waitpid()` for one
  positive pid returns that pid, 0 or -1; an equality test would imply an impossible fourth branch.
- The `WaitFailed` sentence matches `reapChildBlocking()` so an outcome reads the same whichever wait
  produced it.
- The deadline is re-read from the monotonic clock rather than counted in iterations, so a sleep cut
  short by a signal or stretched by load cannot change the bound.
- `poll()` with no descriptors is the sleep because it is already the primitive every other bounded
  wait uses, needs no extra include, and takes milliseconds like every other bound; a sleep
  interrupted by a signal returns early to a loop that re-checks child and clock.

### `PROCESS_GROUP_SETTLE_MS`

- By this point every member has had `SIGTERM` and the leader has been collected; what remains is a
  descendant still finishing its shutdown or one that will not. A short wait distinguishes the two,
  and guessing low only kills a well-behaved descendant a moment early, which is no correctness
  difference because the path ends in `SIGKILL` anyway.

### `PROCESS_GROUP_DRAIN_TIMEOUT_MS`

- `SIGKILL` cannot be caught, blocked or ignored, so this waits for the kernel: each member's teardown
  and its collection by its parent (init or a subreaper). Descendants are not this process's
  children, so `waitpid()` cannot observe them; the group probe is the only observation, and a group
  entry persists while any member, zombie included, remains. Two seconds far exceeds what that takes;
  a group still populated at the end is reported as a leak with its state named.

## tests/L2Tests/test_main.cpp (part 2 of 2)

Detail moved out of the condensed comments in the second half of the L2 runner: the
process-group helpers, the shared terminate-and-reap path, the broken-pipe and process-group
probes, host launch and teardown, the cross-translation-unit seam, `DualPathHostLifecycleTest`,
the global environment and `main()`.

### probeProcessGroup()

- `kill()` with signal number zero performs every check a real signal would, including that the
  target exists, and delivers nothing. Applied to the negated group id it answers "does this group
  still have a member", the only question available about processes that are not this process's
  children. `waitpid()` cannot answer it: a grandchild is not a child, and its exit is reported to
  whoever adopted it.
- An unrecognised errno is reported as `Populated`, never `Empty`, because every caller treats
  `Empty` as "the guarantee holds". A non-positive id is reported as `NotPermitted` for the same
  reason: only a real, confirmed, empty group may produce the answer callers act on.
- A caller that negated the id itself would be asking about a group with a negative id, which
  cannot exist, and would get `Empty` for every group.

### awaitProcessGroupEmpty()

- Uses the same poll interval and monotonic deadline as `waitForChildExitByPolling()`. The
  deadline is re-read from the clock rather than counted in iterations, so a sleep the kernel cut
  short or ran long cannot stretch or shrink the bound. `poll()` with no descriptors is the file's
  sleep.
- `finalState` exists so a caller reporting a failure can name what it found. `false` means the
  bound expired with members still present, or with members this process may not signal.

### terminateAndReapChildProcess()

- **One implementation of the contract.** Free of module-scope state, so every child the harness
  creates ends the same way and "nothing this run started outlives it" has exactly one
  implementation. Callers: `terminateAndReapFakeServiceHost()` and
  `cecL2ProveEpipeDiagnosticAndChildReaping()`. The second drives this function itself, not a copy,
  so the reaping the SIGPIPE disposition makes reachable is tested on the path teardown takes.
- **SIGTERM, then SIGKILL.** SIGTERM first so a child with a handler (the fake service host
  installs one) runs its own clean shutdown. SIGKILL only after the bounded wait, so a wedged child
  cannot survive: an orphaned host would hold the production service name and make the next run
  fail on a stale registration.
- **The bound is unconditional.** The observation descriptor selects only how the grace period is
  spent. With a descriptor the wait is end of file on a pipe the child holds for its lifetime: the
  real event, seen the instant it happens, and the path the broken-pipe seam exercises. Without one
  the same `gracePeriodMs` is spent polling `waitpid(WNOHANG)`, which notices an exit up to one
  `CHILD_EXIT_POLL_INTERVAL_MS` late and reaps in the act of noticing. The final blocking reap is
  bounded because it is reached only after an observed exit or after SIGKILL, which no process can
  catch, block or ignore.
- **History.** An earlier form ran the wait and the escalation only when a descriptor was supplied
  and otherwise let an unbounded blocking reap serve as the wait, on the claim that a channel-less
  child leaves SIGTERM at `SIG_DFL`. That is a property of particular children, not of the function
  (the fake host handles SIGTERM), and it made a termination guarantee depend on how a caller
  assigned the descriptor. A child that handles or ignores SIGTERM is now bounded either way.
- **Why the process group.** `waitpid()` collects one process. A child that forks, or a child that
  is a shell script, leaves processes the harness never created and cannot wait on; they are
  reparented to init, keep running, and keep every inherited descriptor, including copies of the
  pipes this runner observes exits through. Only a signal reaches them, and only through the
  process group, which is why `startFakeServiceHost()` makes its child a group leader.
- **Group sequence.** With a group: SIGTERM to the group, the bounded wait for the direct child,
  SIGKILL where it did not go, the reap of the direct child, then a group probe that must find it
  empty. `kill()` to the group failing with ESRCH is the one observation that the group has no
  members left, which makes "no descendant outlived this run" a checked fact. A group still
  populated after SIGKILL is a failure with its state named.
- **No group.** One pid is signalled, waited for and reaped, and nothing is probed. That is correct
  for a child the caller did not put in a group, such as the broken-pipe probe's child, which
  neither forks nor execs.
- **Parameters.** `description` is e.g. "the fake HDMI CEC AIDL service host".
  `exitObservationFd` is the read end of a pipe whose only write end the child holds; when negative
  the same grace period is served by polling, and the wait is never skipped. `gracePeriodMs`
  applies on both paths. `childProcessGroupId` is a group the caller established and read back.
- **Return.** `true` covers a child reaped by the polling wait or by the blocking reap, with the
  outcome naming how it ended.
- **Postconditions.** No child exists for `childPid`, except where `waitpid()` refused the pid,
  which is reported as `false` because no signal can remedy it. With a group and a `true` return,
  `kill()` to that group fails with ESRCH, so no process the child started, at any depth, remains.
- **Warnings.** The caller owns the descriptors: the two callers hold different ones for different
  lifetimes, and a close here would be invisible to whichever still needed it. A child collected by
  the polling wait is not waited on again, because a second `waitpid()` fails with ECHILD and would
  report a collected child as lost. `childProcessGroupId` must never be assumed: every process
  starts in its parent's group, so an unconfirmed id is the runner's own, and a group signal would
  end the runner, GoogleTest and every sibling in the group. The function compares the id against
  `getpgrp()` and drops to the single pid on a match, but a caller relying on that relies on a
  guard rather than a fact.
- **See also.** `waitForChildExitByPolling()`, `probeProcessGroup()`.
- **Pid check.** Defensive: both callers check first. A signal to a non-positive pid is not a
  no-op (0 addresses this process's whole group, -1 every process the user may signal), so a future
  caller that forgot the check gets a reported failure instead of a runner that killed itself.
- **Group-mode decision.** Made once, from the caller's value and this process's own group. A fork
  inherits its parent's group, so a caller that called the child's pid a group id without reading
  the group back would pass exactly the runner's group. Dropping to the single pid is less than
  asked and is traced, since a descendant could then survive; killing the runner would be
  catastrophic.
- **Double SIGTERM.** The child leads its group, so the group signal reaches it; the pid signal
  covers a child that left its group after confirmation by calling `setsid()` or `setpgid()`. A
  handler running twice is harmless because the host's shutdown is idempotent.
- **Failed SIGTERM.** ESRCH means the child already exited, as on the host's readiness-failure
  path. It must still be reaped, so the failure is traced rather than treated as an error.
- **Wait forms.** The descriptor buys precision, not the bound. End of file stays the preferred
  form and the one the broken-pipe seam drives.
- **Refused wait.** `waitpid()` disowning the pid (ECHILD) means the pid is not this process's child
  or was collected elsewhere. SIGKILL and a blocking reap cannot improve on that, and signalling an
  unowned pid is a hazard because the number may have been reused.
- **Group probe after a refused wait.** Skipped for the same reason SIGKILL is: the group is not
  the harness's to signal, a probe would report on unrelated processes, and the return is already
  `false`.

### terminateAndReapFakeServiceHost()

- Idempotence matters: it runs from SetUp's readiness-failure path and again from TearDown, which
  executes even after a fatal SetUp failure. Clearing `g_hostPid` makes the second call a no-op
  instead of a signal to a pid the operating system may have reused.
- Host-specific steps stay here (the polite `shutdown` request, the channel close, the readiness
  descriptor); signalling, the bounded wait, escalation and reaping are shared. The readiness pipe
  is the death channel because the host holds its write end for as long as it lives.
- The returned outcome is for a caller building a diagnostic.
- Where the launch confirmed the host's process group, no process the host started outlives the
  call either; `terminateAndReapChildProcess()` establishes that by probing the group.
- Nothing launched means the legacy invocation or a launch that failed before forking. A readiness
  descriptor remains only when a launch failed after creating the pipe.
- The `shutdown` request performs exactly the teardown the host's signal path performs, so asking
  first lets the host exit through its documented path with its own trace. It is sent only after
  readiness with the channel open: on the readiness-failure path the host is dead or wedged and a
  request would spend its whole bound. Nothing rests on it; a request can only reach a host that is
  still serving, the one case that never needed a guarantee.
- The control channel is closed before the signal so a host in its command loop sees end of file
  (a clean end of session by its contract) and nothing later writes to a descriptor whose peer is
  going away.
- `g_hostProcessGroupId` is cleared with the pid because both identify a child that no longer
  exists. It is already -1 when the launch could not confirm the group.
- A host that could not be collected is reported with a non-fatal expectation: a leaked process is
  worth saying, but not a reason to cut the rest of a teardown short.

### Broken-pipe probe (section banner)

- **Purpose.** Ignoring SIGPIPE buys the harness two things: the EPIPE arm of
  `writeControlCommand()` becomes reachable, and the teardown that signals and reaps the host still
  runs. Both concern code that runs only once a pipe's reader is gone, which no ordinary invocation
  produces. A look-alike test with its own pipe and raw `::write()` would prove only the kernel's
  behaviour and the signal disposition.
- **Real code.** `writeControlCommand()` takes its descriptor and `terminateAndReapChildProcess()`
  its pid as parameters, so both run against resources the probe owns. They are the functions
  `performHostControlRequest()` and `terminateAndReapFakeServiceHost()` call, with no test-only
  branch, so a change that broke either breaks the probe.
- **Platform needs.** None: no fake host, `/dev/binder`, service manager, back-end or network.
  Writing to a pipe whose last read end is closed returns -1 with EPIPE synchronously, and SIGTERM
  ends a child at `SIG_DFL`, so the probe behaves the same under invocation D (host, no binder) and
  invocation E (binder-capable guest).
- **Why a child.** First, the reader must be provably gone: a fork duplicates every descriptor, so
  the child holds a copy of the read end. The child closes both probe ends and only then writes one
  byte to a handshake pipe, and the parent writes only after reading that byte; the parent closes
  its own read end first, since only the holder can close a descriptor. The race is removed, not
  narrowed. Second, there must be something to reap: a real child collected by a real `waitpid()`.
- **Handshake pipe as death channel.** The child holds its write end for life, so end of file means
  the child exited, the same shape as the host's readiness pipe. That gives
  `terminateAndReapChildProcess()` a real channel, so its bounded wait and SIGKILL escalation are
  the ones teardown uses rather than the (equally bounded) polling fallback.
- **Never touched.** `g_hostControlWriteFd`, `g_hostObserveReadFd`, `g_hostPid`,
  `g_hostReadinessReadFd`, `g_hostReportedReady` and the SIGPIPE disposition. Under invocation E a
  live session depends on them, so the probe owns four descriptors and one child of its own, reads
  the disposition without altering it, and leaves nothing behind on any path.

### EPIPE_PROBE_COMMAND

- Recognisable on purpose and never written to a live channel, so a reader of a diagnostic quoting
  it can tell it came from the probe. Requiring the exact text in the failure sentence proves the
  diagnostic is not generic.

### EPIPE_PROBE_TIMEOUT_MS

- Two seconds, a backstop for three waits: the handshake byte, the write a reader-less pipe rejects
  synchronously, and the exit of a child at `SIG_DFL` for SIGTERM.
- Short where the host's bounds are long because nothing here leaves the kernel's pipe and process
  machinery, while the host's waits cross a process start, a binder driver and a service manager in
  an emulated guest. Far above any scheduling delay, yet short enough that a harness fault is
  reported before a CI job's timeout.

### EPIPE_PROBE_CHILD_EXIT_SIGNAL_SETUP_FAILED

- Value 122, outside the host binary's range and distinct from the two pre-exec codes.
- Claimed only on a path that writes no handshake byte, so the parent reports "the child exited
  before reporting that it had closed the probe pipe" rather than a timeout.

### closeIfOpen()

- A double close is not harmless in a process that opens descriptors afterwards: the number may by
  then belong to something else. Hence the variable is reset to -1, whether it was open or not.

### EpipeProbeResources

- A scope guard rather than a trailing cleanup block: the probe has ten steps and any can fail and
  return, so every return, including an exception escaping a `std::string` operation, passes
  through the same release.
- The child release is a backstop. On success the probe ends its child through
  `terminateAndReapChildProcess()` and clears the pid, because a reap that happened only in a
  destructor would prove nothing about the code teardown runs. The destructor's SIGKILL serves the
  failure paths before that step.
- A copy would duplicate descriptor numbers and a pid, so the second destructor would close and
  signal what the first had already released.

### ~EpipeProbeResources()

- Runs over the same five members every explicit cleanup step uses. `closeIfOpen()` resets each
  descriptor as it closes it and the successful path clears the pid, so nothing is closed twice and
  a reaped child is not signalled again; both a descriptor number and a pid can be reused.
- Descriptors go first and the child last. Their order among themselves does not matter, but
  ending the child is the only step that can block, so no descriptor is held across that wait.
- A surviving child is killed rather than asked to stop: this path is reached only where the probe
  gave up partway, and the orderly ending is what it gave up on. An unreaped child would become a
  zombie outliving the suite, and the harness's guarantee about the host applies equally to a child
  created to test that guarantee.
- On success there is nothing left to do, deliberately: the probe ends its child through
  `terminateAndReapChildProcess()` and closes its descriptors explicitly, because a reap that only
  happened in a destructor would say nothing about the path a teardown takes.
- Nothing it opened leaks into the rest of the run and nothing it forked becomes a zombie.
- Nothing that could raise may be added: an exception escaping while the stack unwinds from a
  failed step would terminate the process instead of failing one case. Its calls are `close()`,
  `kill()`, `waitpid()` and one trace line, none of which raises short of allocation failure; a
  child it could not collect is reported through the trace.

### awaitProbeChildClosedProbePipe()

- Until the byte arrives the child may still hold its inherited copy of the probe pipe's read end,
  and a write to a pipe that still has a reader succeeds. Waiting turns "the child has probably
  closed it by now" into an observed fact. It is a real wait, not a sleep: `poll()` gets the time
  remaining until one monotonic deadline and returns the moment the byte is there.
- End of file means the child exited before reporting (it could not make SIGTERM fatal to itself,
  or it was killed). That is a fault in the probe, not in the property under test, so it is not
  described as a timeout.
- The precondition matters because while the parent holds its copy of the handshake write end, end
  of file can never arrive and a dead child would present as a timeout.

### proveEpipeDiagnosticAndChildReaping()

- **Step roles.** 0 refuses a disposition that would terminate this process; 1-3 build a pipe whose
  reader can be removed and a child that can be reaped; 4 and 5 remove every reader of the probe
  pipe; 6 establishes that the removal is complete; 7 makes the real call; 8 requires the real
  diagnostic; 9 performs the real reaping; 10 releases the rest. Skipping 4, 5 or 6 turns the
  required EPIPE into a successful write.
- **observedDiagnostic** lets the caller assert on the sentence at its own line. It is empty when
  the probe never reached the call, and when the call unexpectedly succeeded, because
  `writeControlCommand()` leaves its out-parameter untouched on success.
- **True return** means the real write reported EPIPE with a diagnostic naming the command, and the
  real reaping collected the child.
- **Step 0** is a refusal, not an assertion: under `SIG_DFL` the write in step 7 would not return,
  and a probe that killed the runner while checking it cannot be killed is the worst outcome. The
  caller checks the same thing and fails the case; this copy cannot be skipped by a future caller.
- **Step 1** sets `O_CLOEXEC` on both ends atomically at creation. Nothing here execs, but a
  descriptor that would survive an exec for no reason is the kind of difference that later becomes
  a bug.
- **Step 2** keeps the probe pipe separate because on invocation E the live control channel's reader
  is a host serving a session the rest of the invocation depends on.
- **Step 3.** The child stays alive until signalled so step 9 has a real child to collect.
- **In the child.** Only calls valid after a fork: `close()`, `sigaction()`, a raw `write()` and
  `_exit()`. Nothing allocates or takes a lock, because a binder threadpool may be running at the
  fork (it is on invocation E) and a fork copies a lock's state as it stood, so a child taking one
  could deadlock. `std::memset` is pure computation over a local. The write end is closed as well as
  the read end because a descriptor nobody needs and nobody watches is how a leak starts.
- **No setpgid().** The host's child is made a group leader because it execs a binary the harness
  does not control, which may fork. This child execs and forks nothing, so it can have no
  descendants. Keeping it out of a group keeps the single-pid path through
  `terminateAndReapChildProcess()` covered on every run of the tier.
- **SIGTERM to SIG_DFL.** The parent ignores SIGPIPE and nothing in the binary touches SIGTERM, so
  the inherited disposition is already the default, but "correct by accident" is no basis for a
  bounded wait. If `sigaction()` fails the child exits with
  `EPIPE_PROBE_CHILD_EXIT_SIGNAL_SETUP_FAILED`.
- **The byte.** Written only after the closes: a byte written earlier would license the parent's
  write while a reader still existed, the race the handshake removes. `writeRawFully()` is the
  file's post-fork write and tolerates a short or interrupted write; its failure is silent by
  contract and shows the parent end of file once the child is killed.
- **pause() loop.** `pause()` returns only when a signal arrives; step 9's SIGTERM or SIGKILL ends
  the child, and the loop stops any other signal from causing an early exit that would leave step 9
  nothing to terminate.
- **Step 4.** While the parent holds a write end open, a dead child produces no end of file, and the
  death channel step 9 relies on would sit out its whole bound diagnosing the wrong thing.
- **Step 5.** A pipe reports EPIPE to a writer only when no read end remains open in any process.
  Leaving the parent's copy open is the easiest mistake here: the write would succeed into the pipe
  buffer and the probe would fail for a reason unrelated to the property under test.
- **Step 7.** A reader-less pipe rejects the write synchronously, so the deadline is a backstop a
  sound mechanism never approaches.
- **Step 8.** The diagnostic is the half of the property a bare errno cannot establish. The command
  text proves the sentence names the command that failed; "EPIPE" proves it took the broken-pipe
  arm, since the deadline arm also names the command.
- **Step 9.** Its bounded wait and escalation are exercised rather than bypassed. This is the second
  half of what ignoring SIGPIPE buys: a runner killed by the signal would never reach a teardown.
  On failure the pid stays set so the guard still tries to kill and collect the child.
- **Step 10.** The ordinary path releases its own resources explicitly, so the guard is visibly a
  backstop; both calls are idempotent and the destructor then finds nothing to do.

### Process-group probe (section banner)

- **Why ordinary runs cannot show it.** The fake service host does not fork, so the group teardown
  normally addresses a group of one and behaves like signalling a pid. The two properties that
  matter only when a host does fork (and a host path that is a shell script forks by its nature)
  have nothing else to exercise them.
- **Descendants do not outlive the run.** A process the host started is not the runner's child, so
  `waitpid()` can neither collect it nor see it go. Before the launch put its child in a group of
  its own, nothing reached such a process, and the only group-wide signal available addressed the
  runner itself. The probe creates a child and a grandchild the runner never forked, and requires
  the grandchild to be gone at the end.
- **The child inherits three descriptors and no others.** A descendant holding a copy of a pipe's
  write end keeps that pipe from ever reporting end of file, which is how a death channel stops
  reporting death. The sweep that closes the rest is invisible from inside the child, so the parent
  hands it a witness pipe it never names, closes its own copy of the write end, and requires end of
  file, which can only arrive once the sweep closed the child's copy.
- **Why the grandchild ignores SIGTERM.** At the default disposition it would die on the group's
  SIGTERM, proving only that the first signal reaches a group and passing just as well against a
  SIGTERM-only teardown. Ignoring SIGTERM forces the path that matters: the direct child exits and
  is reaped, the group still has a member, and the whole group is sent SIGKILL.
- **Platform needs.** None: `fork()`, `setpgid()`, a pipe and a signal, so it behaves the same under
  invocation D (host, no binder) and invocation E (binder-capable guest).
- **Leaves nothing behind.** The guard kills the whole group and reaps the child from its
  destructor, so a probe that fails halfway leaves no more behind than one that succeeds.

### GROUP_PROBE_CHILD_EXIT_SETPGID_FAILED

- Value 123, distinct from `GROUP_PROBE_CHILD_EXIT_FORK_FAILED` (124) and
  `GROUP_PROBE_CHILD_EXIT_SIGNAL_SETUP_FAILED` (125). The parent reports the status verbatim rather
  than translating it, as it does for the host's own codes.

### GROUP_PROBE_HANDSHAKE_TIMEOUT_MS

- Five seconds, the same order of magnitude as the broken-pipe probe's handshake bound and for the
  same reason, on a loaded machine. Expiry means the probe cannot proceed, not that the property
  failed.

### GROUP_PROBE_CLOSURE_WINDOW_MS

- Both uses check a closure that has already happened: the witness pipe, whose write end the
  child's sweep closed before it wrote its handshake byte, and the liveness pipe, checked for the
  opposite outcome (a grandchild still alive means no end of file within the window).

### ProcessGroupProbeResources

- A scope guard for the same reason as `EpipeProbeResources`: any step can report a failure and
  return, and cleanup written at the end would run only on the path where it does not matter.
  Every release is idempotent; the successful path ends the child through
  `terminateAndReapChildProcess()` and clears the pid.
- A copy would duplicate descriptor numbers, a pid and a group id, and the second destructor would
  close and signal what the first released.

### ~ProcessGroupProbeResources()

- SIGKILL goes to the group rather than the pid because this path is reached only where the probe
  gave up partway, and the likeliest leftover is the grandchild built to be hard to kill.
- The group is checked for emptiness afterwards and a leak is traced, because a destructor cannot
  fail a case and a silent leak would be a process outliving the suite with nothing said.
- An exception escaping while the stack unwinds from a failed step would terminate the process
  instead of failing one case.

### awaitOneByte()

- A real wait on the real event rather than a sleep: `poll()` gets what is left of one monotonic
  deadline and returns the moment the byte is there.
- End of file means the writer exited before reporting, a fault in the probe rather than in the
  property under test.

### proveHostProcessGroupTeardown()

- **What it observes.** Both facts are observed, not inferred from a return value: end of file on a
  pipe whose only remaining writer was the process in question, and `kill()` to the group failing
  with ESRCH. Nothing else in this tier can produce either.
- **Real functions.** The child's first two acts (own process group, then the descriptor sweep)
  are exactly what `startFakeServiceHost()`'s child does, in the same order, and step 8 calls the
  same `terminateAndReapChildProcess()` teardown calls, with a group id confirmed the same way. A
  copy of either would prove nothing about the path a teardown takes.
- **True return** means the child led its own group, its sweep closed the witness descriptor, its
  grandchild survived SIGTERM and did not survive the teardown, and the group is empty. The
  resources guard releases everything on the failing paths.
- **Post-fork calls.** The child is held to the same rule as every fork in the file: only
  `setpgid()`, `close()`, `close_range()`, `getrlimit()`, `fork()`, `sigaction()`, a raw
  `write()`, `pause()` and `_exit()`, because a binder threadpool may be running at the fork and a
  fork copies a lock's state as it stood.
- **See also.** `awaitProcessGroupEmpty()`.
- **Step 1.** `O_CLOEXEC` matters for none of the three pipes, since nothing execs, but keeps the
  shape of the launch the probe stands in for.
- **Sweep evidence.** The sweep keeps the handshake and liveness write ends; the parent's
  observation of the witness write end's closure is the only evidence outside the child that the
  sweep ran.
- **Grandchild.** The process the harness never created and cannot wait on. It drops the handshake
  write end and keeps only the liveness write end, so end of file on the parent's read end is a
  statement about it alone; ignoring SIGTERM means the group's first signal cannot end it.
- **Back in the child.** Its liveness write end is closed so the grandchild is the only writer; the
  handshake byte follows, meaning "everything above has happened".
- **Step 3.** Each write end the parent drops is what gives end of file on the matching read end
  its meaning: while this process holds a write end open, no closure elsewhere can produce one.
- **Step 5.** The group is read back exactly as the launch reads it, because a merely assumed group
  id is the runner's own and everything after would be addressed at the runner.
- **Step 6.** The witness write end was held only by this process and the child; this process
  closed its copy in step 3 and never told the child the number, so end of file means the child's
  sweep closed it, which must already have happened because the sweep precedes the handshake byte
  step 4 waited for.
- **Step 7.** Requiring the grandchild alive is what makes step 9 a measurement rather than a
  tautology.
- **Step 8.** The same call, parameters and bounded escalation `terminateAndReapFakeServiceHost()`
  makes. On failure the pid and group stay set so the guard still tries to end them.
- **Step 9.** The fact `waitpid()` could never produce: the grandchild ignored SIGTERM, so it can
  only have gone by the group's SIGKILL.
- **Step 10.** The empty group covers a descendant holding no descriptor of the harness, which
  step 9 could not see.

### launchHostAndWaitUntilReady()

- The whole of the remote mode's preparation. A launch that did not happen, or never became ready,
  means the AIDL invocation cannot be performed; the alternative to failing is a suite that silently
  exercises the legacy back-end and reports the result as AIDL evidence.
- On success the service-side threadpool is also running.
- Callers use `ASSERT_NO_FATAL_FAILURE` so a fatal failure stops them rather than letting them
  continue.
- **Channel check.** `ping` touches nothing in the fake and answers from the host's command loop,
  establishing exactly that a command written here is read there and its reply comes back. Doing it
  in setup gives one diagnostic naming the handoff before any case runs; found later it would be an
  arbitrary case timing out, with every channel user failing after it and nothing naming the cause.
  The likeliest cause, the `FD_CLOEXEC` step in the child, stops the host from starting at all, but
  crossed descriptors or an older host build without the channel present here.
- **Reap before FAIL().** A fatal GoogleTest failure returns from the function at once, so anything
  after it would not run, and the reap supplies the host's exit status, the single most informative
  fact about why no token arrived.

### resolvedAidlMode()

- The single spelling of the "unset or empty means `absent`" rule. `applyAidlModeBeforeInit()`
  acts on its result before `init()`, and `failUnlessSelectedBackEndMatchesMode()` checks the
  selection against the same result after it. It does not validate; `applyAidlModeBeforeInit()`
  fails the run on an unrecognised value.
- It differs from the seam `cecL2RequestedAidlMode()`, which hands the case file the raw value so
  the selection case can tell "unset or empty" from an explicit `absent`.

### applyAidlModeBeforeInit()

- The one place the tier's two invocations diverge. The legacy mode launching no second process and
  this translation unit making no binder call keep a plain `./run_L2Tests` runnable on a host with
  no kernel binder support, where the selection preflight declines before libbinder is reached; the
  remote mode's launch and handshake happen here because here is the only place they still can.
- Unset (or empty) is the single permissive case, because a bare `./run_L2Tests` legitimately means
  "run the legacy arm". A typo does not, and a typo that silently downgraded the run would report a
  green result for an invocation that never happened.
- **Rendered value.** Every earlier arm matched a known spelling, so the final diagnostic is the one
  in the binary naming an unvalidated value, the boundary the log-injection contract applies at.
  Streamed raw, a mode of `$'bogus\n::error::FORGED'` ended the message and began a standalone
  GitHub workflow command on the next line.
- The `absent` trace says this harness makes no binder call, not that the process avoids
  libbinder: on a host with a binder driver node, `init()`'s production selection reaches
  libbinder whatever the mode.

### failUnlessSelectedBackEndMatchesMode()

- The hard failure for a stale registration in mode `absent`. On `remote` the host refuses a taken
  name; `absent` launches no host and performs no lookup, which left a stale "HdmiCec" service free
  to win the selection, the run then failing only through case-level mismatches. Reading the
  outcome after `init()` closes that gap with no binder call from this translation unit.
- Identity is a `dynamic_cast` against `DriverImpl`, whose header includes no binder or AIDL
  header. `Driver::getInstance()` returns only `DriverImpl` or `DriverAidlImpl`, so "not
  `DriverImpl`" is the AIDL back-end; `remote` requires exactly that, and its failure points at the
  factory's "not usable" line, which records why the AIDL back-end was declined.
- The service name is a literal in the diagnostic because this translation unit cannot include
  the generated interface that spells it.
- The failure is raised in the global environment's `SetUp()`, so no case body runs and the binary
  exits non-zero; `TearDown()` still runs and reaps any host.

### Cross-translation-unit seam (section banner)

- **The four functions.** Two drive and observe the out-of-process fake service; the third drives
  the harness's own control-channel write and child reaping; the fourth hands the case file the
  `CEC_TEST_AIDL_MODE` value, so this harness stays the tier's only reader of the variable.
- **Why here.** Every descriptor and child process in the binary is owned by this harness (it
  creates the pipes, hands their far ends to a child, signals, reaps and closes), and its lifecycle
  is the only place that knows whether a host exists. The case file uses all of that but owns none
  of it, so only these entry points cross. The case proving a reader-less control write reports
  EPIPE, and that the child making it reader-less is still reaped, cannot own the pipes or the child
  either.
- **No header.** A header for four functions used by one file would be a new build file, and
  nothing test-scope may reach a production or installed surface. The case file declares them with
  matching extern declarations instead.
- **Drift.** The exported C++ name encodes each function's parameter types (`std::string`
  references, or none), so a one-sided parameter change is an undefined symbol at link time
  (`make -C tests/L2Tests` fails and names the function). The return types (`bool`, and
  `std::string` for `cecL2RequestedAidlMode()`) are not part of an ordinary function's mangled name,
  so a one-sided return-type change still links and both declarations are kept in step by hand.
  Superseded: the contract text was formerly written out in full above both declarations, kept in
  step by editing both together; both comments are now condensed, and the full contract is recorded
  in these notes.
- **What a case may assume.** A `true` return from the request function means one command was
  written and one reply line read back, beginning "OK " or "ERR ". It may not assume success: an
  "ERR " line is a successful exchange and reports `true`, because whether a refusal is expected is
  the case's judgement. It may not assume a channel exists: on the legacy invocation there is no
  host, so `cecL2HostControlChannelIsOpen()` reports `false` and every request fails with a
  diagnostic rather than blocking. The broken-pipe entry point depends on none of this, bringing
  its own pipes and child, so it means the same on both invocations.

### cecL2HostControlChannelIsOpen()

- On the legacy invocation there is no second process, so there is nothing to ask.
- `true` requires both descriptors open and an answered `ping` during setup.
- Skipping on `false` would hide a broken handoff behind a green run, the exact failure the tier
  exists to rule out.

### cecL2HostControlRequest()

- The protocol is stated normatively in the file block of
  `mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp`; that text is the authority.
- **Bounded on every path.** The write waits for the pipe to accept bytes and the read for one
  newline, both against one monotonic deadline of `HOST_CONTROL_REPLY_TIMEOUT_MS` from entry; an
  interrupted call resumes against the same deadline. A host that exited is reported at once from
  EPIPE or end of file; a live, silent host when the bound expires. A harness that could hang would
  be killed from outside with no result recorded, which is worse than any failed assertion.
- **command** is e.g. "sent-count". It must be non-blank; a newline or carriage return is rejected
  rather than sent because either would desynchronise the one-reply-per-command framing for every
  later request.
- **False return** covers no channel, a malformed command, an expired bound, an exited host, or an
  unclassifiable reply; `failureDetail` says which and what it means.
- **Pipe, not binder.** Binder is the transport under test; evidence carried over it would attest
  to the transport with the transport, and a fault could corrupt the evidence invisibly. The channel
  is two inherited pipes and works the same whether the driver is healthy, degraded or absent.

### cecL2ProveEpipeDiagnosticAndChildReaping()

- The only entry point not about the fake service host. Ignoring SIGPIPE buys two things, the
  reachable EPIPE arm of `writeControlCommand()` and a teardown that still reaps the host, and no
  ordinary invocation exercises either; a look-alike with its own pipe and raw `write()` would leave
  both unexercised. It drives the real `writeControlCommand()` (which takes its descriptor as a
  parameter for this reason) and the real `terminateAndReapChildProcess()` used by the global
  TearDown, with no copy and no test-only branch.
- **Sequence.** Creates a handshake pipe and a probe pipe; forks a child that closes both probe
  ends, reports over the handshake pipe, makes SIGTERM fatal to itself and blocks; closes the
  parent's handshake write end and probe read end so no reader remains; waits, bounded, for the
  report; requires `writeControlCommand()` on the probe write end to fail with a sentence naming the
  command and reporting EPIPE; ends the child through `terminateAndReapChildProcess()` and requires
  the reap; releases every descriptor.
- **Determinism.** Nothing is slept on and no wall clock is polled; each of its three waits is a
  real wait on a real event under one bound, so it behaves the same under invocation D (no binder)
  and invocation E (binder-capable guest).
- **observedDiagnostic** lets the caller assert on the sentence at its own line rather than trust
  this function's check; it is empty when the call was never reached or unexpectedly succeeded.
- **Precondition.** Verified as the first act and reported as a failure rather than written into: a
  probe that terminated the runner while proving it cannot be terminated is the worst outcome.
- **Live state.** It touches none of `g_hostControlWriteFd`, `g_hostObserveReadFd`, `g_hostPid` or
  `g_hostReadinessReadFd`, so it is safe while a real host session is open, as under invocation E.
- **Limit.** It cannot establish what happens without the disposition installed, since that would
  terminate the process. The caller reads the disposition back and fails fatally on `SIG_DFL`
  before calling; that assertion and this function are two halves of one property.

### cecL2RequestedAidlMode()

- **Why it exists.** Only the two `test_main.cpp` files read `CEC_TEST_AIDL_MODE`, yet
  `DualPathSelectionTest.TheResolvedBackEndMatchesTheModeTheHarnessWasGiven` must hold the
  resolved back-end against the requested mode; it asks here instead of reading the environment.
- **Value.** The raw value, read through the same `::getenv(AIDL_MODE_VARIABLE)` call
  `applyAidlModeBeforeInit()` makes, or empty when the variable is unset. It is not normalised:
  unset and empty both come back empty, where `applyAidlModeBeforeInit()` treats them as `absent`,
  so the case can tell "no request" (it skips) from an explicit `absent`.
- **Stability.** Nothing in the runner sets, changes or clears the environment, so the value is the
  one `applyAidlModeBeforeInit()` acted on before `LibCCEC::init`.
- **Live state.** It needs no host, binder or service manager and touches no channel state, so it
  means the same under invocations D and E.

### DualPathHostLifecycleTest.TeardownEndsTheWholeProcessGroupAndInheritsOnlyNamedDescriptors

- **Translation unit.** Everything it drives (the launch's process-group creation, its pre-exec
  descriptor sweep, terminate-and-reap's group-wide escalation) is internal to this file; a case
  elsewhere would need a bridge, a second surface to keep in step for no gain.
- **A case, not a SetUp check.** A SetUp check runs before the suite, reports against whatever
  assertion is in scope, and cannot be named, filtered or seen in the results. This is a named
  property, and it is the only case in either tier that fails if the group teardown regresses.
- **DualPath prefix.** The runner uses one filter per invocation, the DualPath glob; a fixture
  named otherwise would be registered but neither selected nor excluded, which the runner's
  selected-plus-excluded-equals-registered reconciliation would rightly fail on.
- **What it asserts.** Neither half is reachable from the ordinary invocations, where the fake host
  neither forks nor receives an unused descriptor. A SIGTERM-ignoring grandchild the runner never
  forked does not outlive teardown, evidenced by end of file on a pipe it last wrote and by `kill()`
  to the group failing with ESRCH. A descriptor never named to the child is closed before reaching
  anything the child starts, observed from outside as end of file on a pipe whose write end this
  process handed over silently and then released.
- It runs and means the same thing under every invocation of the tier.

### CecL2TestEnvironment::SetUp()

- **Postcondition detail.** The back-end selection is resolved and fixed for the lifetime of the
  process.
- **Order.** The init call is the first thing in the binary that forces `Driver::getInstance()`,
  which resolves the selection once and for all; a service reachable only after it is one the
  middleware never looks for. Moving the mode handling below init would not fail: it would pass on
  the legacy back-end and report the run as AIDL evidence.
- A fatal failure in SetUp exits non-zero, and because TearDown still runs, teardown tolerates a
  partial setup.
- **SIGPIPE first.** A control channel whose write can terminate the process is not one to proceed
  with: the failure it should report would be recorded nowhere and the host reap would not run.
  This is the one step that must precede everything, and a hard failure because the binary's
  evidence rests on what follows. The reasoning, the scope decision and the audit of every write in
  the file are above `ignoreBrokenPipeSignal()`.
- **Ordering trace.** Printed in the run's own log rather than inferred from source: it cannot
  print until the readiness wait has returned, and init cannot run until it has printed.
- **Asserted init.** The environment's SetUp runs once per process, so the one condition that would
  make a second initialization harmless cannot arise. What init raises is real:
  `Driver::getInstance().open()` refused by the selected HAL, or `Bus::start()` failing. Swallowing
  either reports a green suite for a process that never initialized, every case then asserting
  against an unopened stack.
- **Checked selection.** Immediately after init, `failUnlessSelectedBackEndMatchesMode()` holds the
  resolved selection to the mode, so a stale registration under `absent`, or a legacy selection
  under `remote`, fails the run before a case executes rather than through case-level mismatches.

### CecL2TestEnvironment::TearDown()

- **Postcondition detail.** Includes both ends of the control and observation channel.
- **Order.** `term()` reaches `Driver::close()`, which on the AIDL back-end is a transaction to the
  host, so the host must still be serving; reaping first would turn an orderly shutdown into a
  failed close against a process already gone. The channel is likewise closed inside the host
  teardown, after `term()`, so a case's last observation and the library's own close both happen
  while the host is alive.
- **Partial setup.** `term()` raises when the library was never initialized, and the host reap is a
  no-op when no host was launched or one was already reaped on the failure path. Failures are
  reported non-fatally so a first problem cannot hide the cleanup after it.
- **Non-fatal term().** TearDown runs after the suite, so every result is recorded; a fatal
  assertion would obscure legitimately earned results and cut the cleanup short. After a failed
  SetUp the `term()` failure is honest, correctly saying the process never came up.
- **Disposition last.** The host teardown closes the control channel and makes its own last request
  over it, all of which need the protection being removed. A disposition is process-wide and
  outlives the environment, so restoring the original keeps the change scoped to the window that
  needed it instead of leaving it to a static destructor or an `atexit` handler.

### main()

- A non-zero exit includes the case where the global environment failed to come up.
- Configuration comes from the environment, never the command line, so every flag on the binary
  remains GoogleTest's own.

## tests/L2Tests/ccec/test_DualPathIntegration.cpp (part 1 of 2)

### HDMI_CEC_L2_DUALPATH

- The file registers 16 cases: `DualPathSelectionTest` 4, `DualPathLegacyFlowTest` 6, `DualPathAidlFlowTest` 6. The group's `@brief` carries no count.
- Superseded: the pre-refine group `@brief` named "the 14 cases" of this file, when `DualPathAidlFlowTest` held 4. `EnablingTheDriverRegistersOneAddressThatLibCcecReadsBackThroughTheHal` and `LibCCECReportsTheFixedPhysicalAddressWithoutCrossingBinder` brought it to 6.

### File overview (`@file`)

**Flows asserted on each back-end.**

- Flow A, inbound, legacy: HAL Rx callback -> `DriverImpl::DriverReceiveCallback` -> receive queue -> Bus reader thread -> Connection address filter -> `FrameListener::notify` -> `MessageDecoder::decode` -> the typed `MessageProcessor::process` overload.
- Flow A, inbound, AIDL: fake service (separate process) -> `IHdmiCecEventListener::onMessageReceived` on a binder thread -> the same receive queue, Bus reader thread, address filter, `FrameListener::notify`, decoder and typed overload.
- Flow B, outbound, legacy: typed message -> `MessageEncoder` -> `Connection::sendTo` -> Bus -> `DriverImpl::write` -> HAL `HdmiCecTx`, with the exact bytes on the wire.
- Flow B, outbound, AIDL: typed message -> `MessageEncoder` -> `Connection::sendTo` -> Bus -> `DriverAidlImpl::write` -> `IHdmiCecController::sendMessage` across the binder driver to the host process.
- Only the producing thread and the transport change. `EventQueue` is already the receive path's cross-thread synchronization point, so the Bus reader and every layer above it are identical on both arms. That is the claim this tier tests, and the reason the migration reaches no file above the Driver seam.

**Why this is an L2 tier and not an L1 case.**

- libbinder resolves a service registered in the calling process to the local `BBinder`, so `interface_cast` returns that object: no `Bp*` proxy, no transaction across the binder driver, no client threadpool. An in-process fake cannot prove the transport. Hosting the same fake in a separate process makes the middleware hold a real proxy and receive callbacks on a binder threadpool thread, which is why the fake-service host binary exists and why L1 invocations B and C cannot substitute for invocation E.
- Conversely, halcompat's compatibility check reads `getInterfaceHash()` and `getInterfaceVersion()`. On a local object these dispatch virtually to the fake's overrides. Across binder they reach the fake's `onTransact()` override in the host process, which counts them and delegates to the generated dispatch, and that answers both from the compiled-in `VERSION` and `HASH` without calling the overrides; the host's control channel also has no command that installs a hash or version. The compatibility-rejection branches are therefore reached only in-process, so they have no L2 counterpart.

**One process, one outcome.**

- `tests/L2Tests/test_main.cpp`'s global `::testing::Environment::SetUp` calls `LibCCEC::getInstance().init("CEC_TEST")`, the first call in the binary to force `Driver::getInstance()`. Its helper constructs both back-ends, asks the AIDL one whether its service came up, and emits exactly one selected-path line naming the winner. By the first `TEST_F` body the choice is made and immutable.
- A test body can observe the arm but never change it: one binary, two invocations differing only in `CEC_TEST_AIDL_MODE`, and the two arm-specific fixtures skip rather than adapt when the resolved back-end is not theirs.

**Why the selected-path log line is not asserted in a test body.**

- `CCEC_LOG` writes to stdout with `printf`, prefixed `[_CEC_LOG_PREFIX]` and gated on the file-static `cec_log_level` in `ccec/src/Util.cpp` (default `LOG_INFO`, so the line prints by default). The factory emits it during the global environment's `SetUp`, before any test body. No `TEST_F` can capture it retroactively, and it cannot be re-triggered: the selection is resolved and the emitting helper has internal linkage in another translation unit.
- The coverage runner greps each invocation's captured log instead. In-process, back-end identity is asserted by `dynamic_cast` against the two non-installed concrete headers, which is authoritative and needs no log.
- No introspection API is added to the `Driver` interface for this, and none may be: it would grow the middleware public surface.

**What is covered.**

- The middleware leg of both flows on the legacy back-end; both flows on the AIDL back-end (flow B over a real proxy and a real driver transaction, flow A over a real binder callback thread); the selection itself (which back-end resolved, that it is stable across repeated factory calls, and that it matches the harness's mode); and one harness guarantee both arms depend on: a write to a pipe whose reader has gone reports `EPIPE` to the case that asked instead of terminating the runner before its teardown can reap the host.

**What is not covered: the plugin leg.**

- The plugin leg (HdmiCecSink/Source `FrameListener` and its typed handlers) cannot be joined to this leg by any test-only change: the plugin L1 and L2 binaries link entservices-testframework's CEC mock (`Tests/mocks/HdmiCec.h`) in place of this middleware, so they contain no back-end.
- Required change, reported and not made: build the plugin test binaries against the real hdmicec libraries (`libRCEC`/`libRCECOSHal`) instead of the framework CEC mock — add the middleware include path, link the two libraries in the plugin test CMakeLists, and drop the `-include` of `Tests/mocks/HdmiCec.h` for those targets. Only then can one test span HAL -> middleware -> plugin. Those files are outside this migration's scope.

**How inbound delivery on the AIDL arm is made coverable.**

- The fake service lives in the host process and is not linked into this runner (that separation is the tier), so this file cannot call it, and `FakeHdmiCecController::sendMessage` records the frame and returns its canned status with no loopback.
- The host serves a control and observation channel: two inherited pipe descriptors, named to the child by `CEC_FAKE_HOST_CONTROL_FD` and `CEC_FAKE_HOST_OBSERVE_FD`, over which it reads newline-terminated commands and writes exactly one reply line per command. `tests/L2Tests/test_main.cpp` creates the pipes, clears `FD_CLOEXEC` on the child's ends between `fork()` and `exec()`, exports the two numbers, and exposes the request/reply call through the cross-translation-unit seam.
- The channel is a pipe, not binder, because binder is under test on invocation E: evidence carried over binder would assert the transport with itself.
- `deliver <hex>` makes the host fire `onMessageReceived` on the listener `FakeHdmiCecService` captured during `open()`, arriving in this process on a binder threadpool thread. `sent-count` and `last-sent` report the application frames the fake actually received (allocation polls excluded: the fake records them separately in `getAllocationPolls()`, which the host does not expose, while `calls` counts them among the `sendMessage` transactions), so a `sendMessage` that never arrived or arrived corrupted is caught. `open-count` and `close-count` report the fake's own session lifecycle, catching an open that never crossed the driver, a session closed behind a case's back, or a `term()` that closed nothing on the far side. `registered` reports the addresses registered through the fake controller, and `calls` reports every IHdmiCec and IHdmiCecController transaction the fake has received, per method, so an address read answered from a cache or a physical-address query that crossed the driver is caught. No inbound AIDL case skips inside its own arm.

**AIDL `close` mapping (B2).**

- `close` on the AIDL arm maps to `IHdmiCec.close`, a high-confidence candidate pending owner confirmation because `HdmiCecClose` has no mapping-table entry. Every case closes its own `Connection`, which does not reach `Driver::close`; only the global environment's `term()` and the state-guard case (which cycles the library and carries the marker in its own doc block) do. A green result does not confirm the mapping.

**Superseded statements.**

- Superseded: the block stated that `getPhysicalAddress` on the AIDL arm is blocked on B1 (the device-settings HAL contract that was not supplied), is deliberately not asserted on invocation E, and is covered on the legacy arm by L1. The AIDL back-end now returns the fixed physical address 1.0.0.0 (`0x01000000`) in every driver state without any AIDL call; the legacy back-end still reads it through `HdmiCecGetPhysicalAddress`.

**The one inheritance from the L1 template that is rejected.**

- The L1 template reaches the stack through `DriverImpl::DriverReceiveCallback`, installed by `restoreDriverInboundRoute()` unconditionally in its fixture's `SetUp`. That callback resolves its target with `static_cast<DriverImpl &>(Driver::getInstance())`; once the factory can return a `DriverAidlImpl` (on invocation E it does) the cast is ill-typed: undefined behaviour with no diagnostic and no bounded consequence.
- The hazard is live in this tier: `tests/L2Tests/test_main.cpp` installs the legacy `HdmiCecDriverMock` on both arms, so `mock->rxCallback` is writable on invocation E, and a fixture that installed the route without checking would arm the cast on the arm where it is wrong.
- So the legacy fixture confirms the legacy back-end before installing the route, never after, and the AIDL fixture never touches the mock's callback members. Reordering those two steps is the most dangerous edit that can be made to this file.

**Self-sufficiency.**

- Every case establishes its own preconditions and leaves no shared state altered: it opens its own `Connection`, registers its own listener, and clears mock expectations in `TearDown`. The L1 suite this file derives from contains cases that fail under `--gtest_shuffle` because they depend on each other.
- Cleanup is RAII, not a trailing call: a fatal assertion returns from the body immediately, so a trailing `removeFrameListener()` and `close()` do not run on the failing path, and `Connection::~Connection()` is empty. Every case holds its connection in a `ScopedConnection`; the library-cycling case also holds a `ScopedCecLibraryCycle`.
- Measured on invocation D for the nine cases other than `WriteControlCommandReportsEpipeAndTheChildIsStillReapedInsteadOfKillingTheRunner`: each passes alone under `--gtest_filter`; the whole `DualPath*` suite passes under `--gtest_shuffle` at two seeds and under `--gtest_repeat=2`; with two fatal assertions deliberately forced to fail, the remaining seven pass and the process still tears the library down cleanly.
- The broken-pipe case's independence rests on construction: it reads the `SIGPIPE` disposition without altering it, creates, drives and releases its own two pipes, child and third pipe, consults and alters no shared state, sleeps for nothing, polls no clock, and leaves no descriptor or child behind on any path.

**Execution environment.**

- The translation unit compiles and links anywhere the middleware does. Invocation D needs nothing special; invocation E needs a binder-capable kernel, a binder protocol version matching the linked libbinder, and a running servicemanager, so it belongs to `.github/workflows/aidl-path-tests.yml` on the binder-capable guest.
- Measured on the authoring host: kernel 6.12.85+, `CONFIG_ANDROID_BINDER_IPC` not set, `binder` absent from `/proc/filesystems`, no `/dev/binder` node; invocation D was executable there and invocation E was deferred. No unexecuted result and no timing figure is claimed.

### Case manifest

- The coverage runner gates every invocation on a zero exit status, an executed test count matching the filter's expected count, and the selected-path line in the captured log. The runner measures the counts from the built binary; the tier's `DualPathHostLifecycleTest` in `tests/L2Tests/test_main.cpp` is also selected by `DualPath*`.

| Fixture | Cases | Executes under | Skips under |
|---|---|---|---|
| `DualPathSelectionTest` | 4 | D and E | nothing |
| `DualPathLegacyFlowTest` | 6 | D | E (fixture `SetUp`, back-end check) |
| `DualPathAidlFlowTest` | 6 | E, all six | D (fixture `SetUp`, back-end check) |
| Registered in this file | 16 | | |

- The fourth selection case, `WriteControlCommandReportsEpipeAndTheChildIsStillReapedInsteadOfKillingTheRunner`, is about the harness: it drives the harness's own `writeControlCommand()` against a descriptor whose reader has gone, requires the command-specific `EPIPE` diagnostic, and requires the child that made the descriptor reader-less to be reaped through the same terminate-and-reap a teardown performs. It builds its own pipes and child, needs no host, driver or service manager, and executes under both invocations.
- `--gtest_filter=DualPath*` selects the suite; the three fixtures share the prefix to match the sibling convention in `tests/L1Tests/ccec/test_DriverAidl.cpp`.
- The registered total is identical for D and E because the arm-specific fixtures skip rather than fail: D = 4 selection pass + 6 legacy pass + 6 AIDL skip; E = 4 selection pass + 6 legacy skip + 6 AIDL pass. Skip identities follow from the fixture guards: under D only the six `DualPathAidlFlowTest` cases, under E only the six `DualPathLegacyFlowTest` cases. Adding an unconditional case moves the passing count and no skip identity.
- Measured invocation D (file's own cases): 16 tests from 3 suites ran, 10 passed, the 6 `DualPathAidlFlowTest` cases skipped, exit status zero, with "back-end selected : legacy" preceded by "the binder transport is unavailable on this platform" — the fallback-not-abort requirement visible in the run's own output. With `DualPathHostLifecycleTest` the tier registers 17 cases in 4 suites: invocation D ran 17, 11 passed and the 6 `DualPathAidlFlowTest` cases skipped, exit status zero; invocation E, measured in a binder-capable guest, ran 17, 11 passed and the 6 `DualPathLegacyFlowTest` cases skipped.
- Superseded: an earlier measurement of 14 tests from 3 suites (10 passed, 4 skipped) under D predates the two added `DualPathAidlFlowTest` cases, and the invocation E split it gave was derived from the fixture guards rather than measured.
- There are exactly three `GTEST_SKIP` sites. Two are opposite-arm fixture skips: `DualPathLegacyFlowTest::SetUp` skips when the resolved back-end is not legacy (fires under E, takes all six cases), and `DualPathAidlFlowTest::SetUp` skips when it is not AIDL (fires under D, takes all six). The third is inside `DualPathSelectionTest.TheResolvedBackEndMatchesTheModeTheHarnessWasGiven` and is not an arm skip: it fires only when `CEC_TEST_AIDL_MODE` is unset or empty; the matrix sets the variable for D and E, so it fires under neither, and its appearance means the runner did not export the variable — a harness fault to treat as a failure, not to allowlist.
- The two inbound AIDL cases, `InboundFrameFromTheFakeServiceArrivesOnABinderThreadAndReachesTheTypedProcessor` and `AFrameDeliveredWhileTheDriverIsNotOpenedIsRejectedByTheStateGuard`, execute under E through the host's control channel; an E run reporting any AIDL case skipped is a defect.
- The whole skip allowlist: under D the six `DualPathAidlFlowTest` cases, under E the six `DualPathLegacyFlowTest` cases; the third site belongs in none.
- `DualPathAidlFlowTest::SetUp` asserts, rather than skips on, the host's channel being open: under E the AIDL back-end resolved, so a host was published and ready and the harness proved its channel with a ping. A closed channel is a harness or host defect; skipping would let E report green with every observation it exists to make not made.
- Selected-path log literals, transcribed from `ccec/src/Driver.cpp` (whose constants live in an anonymous namespace and cannot be imported) so that the runner's grep and a human reader agree on one string; exactly one appears per process:
  - invocation E (AIDL selected): `Driver::getInstance : HDMI CEC HAL back-end selected : AIDL`
  - invocation D (legacy selected): `Driver::getInstance : HDMI CEC HAL back-end selected : legacy`
- Each is emitted at `LOG_INFO` through `CCEC_LOG`, on stdout behind the CEC log prefix and a timestamp, so grep for the trailing substring. On the legacy arm one of three lines precedes it, from one format string with the reason substituted: "...the binder transport is unavailable on this platform", "...the binder transport is reachable but no compatible service resolved", "...the service query failed unexpectedly, so no usable service could be established". They are worded unlike the selected-path line so that its grep yields one hit per process. The first is what invocation D produces on a host without a binder driver; the third is the catch-all when the availability query itself failed, to be read as a query fault rather than an ordinary fallback.
- The manifest was declared the authority for the file's case set: if the case set changes, the manifest changes with it.

### Include rationale

- `<thread>`: `std::this_thread::get_id()` and `std::thread::id`. Invocation E must show a frame arrived on a binder thread, which a test can establish only by comparing the delivering thread with the test-body thread; `DecodingFrameListener` records it because the test thread is blocked in the wait while delivery happens.
- `<cstdlib>`: `std::strtol`, which the host-reply parsers use to read counts and addresses. Nothing in the file reads the environment; `tests/L2Tests/test_main.cpp` reads `CEC_TEST_AIDL_MODE`, acts on it before the selection resolves, and hands the value to the one case that needs it through `cecL2RequestedAidlMode()`.
- `<cstring>`: `std::memset` zeroes a `struct sigaction` before it is filled in; `std::strerror` turns a failing call's `errno` into a sentence.
- `<cerrno>`: `strtol()` reports a range error only through `errno`, so a parser that skipped the check would accept an out-of-range count; the broken-pipe case reads it to establish its own write failed with `EPIPE`.
- POSIX headers: `sigaction()` from `<csignal>` reads back the disposition the harness installed; `pipe2()` and `close()` from `<unistd.h>` and `O_CLOEXEC` from `<fcntl.h>` serve the direct demonstration in the broken-pipe case's third step. The two pipes and the child that drive the harness's `writeControlCommand()` and reaping belong to `tests/L2Tests/test_main.cpp`, reached through the third seam function. Nothing else in the file touches a raw descriptor, and no case opens a file, a socket or a process.
- `ccec/LibCCEC.hpp`: `ccec/Connection.hpp` already includes it, so the line adds no symbol; it is named because the file calls `LibCCEC` directly: `ScopedCecLibraryCycle` calls `term()` and `init()` for the library-cycling case, `EnablingTheDriverRegistersOneAddressThatLibCcecReadsBackThroughTheHal` calls `getLogicalAddress(1)`, and `LibCCECReportsTheFixedPhysicalAddressWithoutCrossingBinder` calls `getPhysicalAddress()`. Library initialization (`LibCCEC::init` resolves the selection in the global environment) is also every case's precondition.
- `../../../ccec/src/DriverImpl.hpp` and `../../../ccec/src/DriverAidlImpl.hpp`: `ccec/src` is not, and deliberately is not made, an `AM_CPPFLAGS` include root, so both are reached by relative path, as the L1 units do from `tests/L1Tests/ccec/` at the same depth. Neither is installed (both are absent from `nobase_include_HEADERS` in `hdmicec/Makefile.am`), which lets a second back-end exist without altering the public API and makes naming the concrete types in a test legitimate. They provide (1) the concrete type names for `dynamic_cast` back-end identity, with no production introspection API; (2) the address of `DriverImpl::DriverReceiveCallback` for `restoreDriverInboundRoute()`; (3) `DriverAidlImpl` as the other `dynamic_cast` target. Including `DriverAidlImpl.hpp` does not make the file an AIDL client: it constructs neither back-end, calls no AIDL method, touches no `android::sp<>` and reaches no service manager; the generated stubs and binder SDK headers arrive only because the header's session members are complete-type `android::sp<>` members.

### Cross-translation-unit seam

- The four functions are defined in `tests/L2Tests/test_main.cpp`, which owns the host's lifecycle, every pipe and child in the binary, and the tier's only read of `CEC_TEST_AIDL_MODE`. The contract is written out on both sides deliberately so a reader of either sees the whole agreement; if one is edited, both are.
- The Itanium C++ ABI mangles each function's parameter types (here the `std::string` references, or none) into its exported name, so a one-sided parameter change is an undefined symbol at link time. The return type of an ordinary function (`bool`, or `std::string` for `cecL2RequestedAidlMode()`) is not mangled: a one-sided return-type change still links, so the two sides must be kept in step by hand, which is why the contract is written out on both. An `extern` declaration is used instead of a header because a header for functions used by one file would add a file to the build, and nothing test-scope may grow a production or installed surface.
- Outbound: a transmit that crossed the binder driver is visible in this process only as "`sendTo` did not throw". The bytes the service received are recorded by the fake in the host process, which is deliberately not linked here (linking it would resolve the service name locally and turn the tier back into the in-process case), so only the host can tell a corrupt or missing send from a correct one.
- Inbound: only the fake can invoke the middleware's `IHdmiCecEventListener`; without a way to ask the host to fire a callback, no case could cause an inbound delivery.
- The evidence travels over two ordinary inherited pipes, not binder, so a transport fault cannot invisibly corrupt it; the pipes behave identically whether the driver is healthy, degraded or absent.
- The command vocabulary is normative in `mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp`. This file uses `listener`, `sent-count`, `last-sent`, `open-count`, `close-count`, `registered`, `calls` and `deliver <lowercase-hex>`. Every wait is bounded against one monotonic deadline, so an exited or silent host yields a failed assertion naming the command, never a hung suite.

### cecL2HostControlChannelIsOpen()

- On the legacy invocation there is no second process, so it reports false.
- Skipping on false would hide a broken handoff behind a green run, which is why `DualPathAidlFlowTest::SetUp` asserts it once for every case in the fixture.

### cecL2HostControlRequest()

- The write waits for the pipe to accept bytes and the read waits for one newline, both against one deadline taken at entry; an interrupted call resumes against the same deadline. An exited host is reported at once from `EPIPE` or end of file; a live, silent host when the bound expires. A hung harness is killed from outside with no result recorded, which is worse than any failed assertion.
- A blank command, or one containing newline or carriage return, is rejected unsent because it would desynchronise the one-reply-per-command framing for every later request.
- `failureDetail` receives a sentence naming what went wrong and what it means.
- An `ERR ` reply reports true; whether the refusal was expected is the calling case's judgement, which the `askHost*` helpers make by checking the reply text.

### cecL2ProveEpipeDiagnosticAndChildReaping()

- It is the only seam not about the fake service. Ignoring `SIGPIPE` buys the harness two things — the `EPIPE` arm of `writeControlCommand()` becomes reachable, and the teardown that signals and reaps the host still runs — and no ordinary invocation exercises either, because a healthy host reads its control descriptor until teardown closes it. A look-alike built from its own pipe and a raw `::write()` would establish only the kernel's behaviour and the signal disposition.
- It drives the real code: `writeControlCommand()`, which takes its descriptor as a parameter for this reason, and `terminateAndReapChildProcess()`, the function the global environment's `TearDown` uses on the host. Neither is a copy or has a test-only branch.
- Steps: create a handshake pipe and a probe pipe; fork a child that closes both ends of the probe pipe, reports that over the handshake pipe, makes `SIGTERM` fatal to itself and blocks; close the parent's handshake write end and probe read end so no reader of the probe pipe remains; wait, bounded, for the child's report (until then the child may hold the inherited read end and a write would succeed); call `writeControlCommand()` on the probe write end and require a failure sentence naming the command and reporting `EPIPE`; end the child through `terminateAndReapChildProcess()` and require the reap to succeed; release every descriptor on every path.
- Deterministic with no platform support: `close()` of a pipe's last read end followed by `write()` returns -1 with `EPIPE` synchronously, and `SIGTERM` ends a child at `SIG_DFL`. Each of the three waits is a real wait on a real event under one bound, with no sleep or wall-clock polling, so it behaves identically under D and E. It touches none of the live channel's state and is safe while a real host session is open.
- `observedDiagnostic` lets the case assert the sentence's substance at its own line rather than trusting the seam's own check; it is empty if the probe never reached the write or the write unexpectedly succeeded.
- The case also asserts the `@pre` fatally first: a probe that terminated the runner while proving the runner cannot be terminated would be the worst outcome.
- It cannot establish what happens without the disposition installed, because that would terminate the process; the case's fatal check on `SIG_DFL` and this seam are two halves of one property.

### cecL2RequestedAidlMode()

- It keeps the harness contract that only the two `test_main.cpp` files read `CEC_TEST_AIDL_MODE`: `TheResolvedBackEndMatchesTheModeTheHarnessWasGiven` needs the requested mode, and asks the harness for it instead of reading the environment.
- It returns the raw value, empty when the variable is unset, so the case skips on unset or empty exactly as it would on the variable itself and compares every other value verbatim.

### restoreDriverInboundRoute()

- After the call an injected frame travels the production route HAL -> `DriverImpl` -> Bus -> `Connection` rather than reaching whatever callback was registered last.
- The mock stores whatever the real `HdmiCecSetRxCallback` entry point is handed, on an instance that outlives every fixture. The global environment creates the mock and initializes the library but does not install the driver's registration; `DriverImpl::open()` registers through the mock only on the first open and returns early while OPENED. Any case that registers its own callback therefore leaves injections reaching it, through a data pointer that may have died with its stack frame.
- The L1 tier measured the consequence: `--gtest_repeat=2` over a filter mixing a callback-registering case with the inbound integration cases passed iteration 1 and failed every inbound case in iteration 2. Restoring the route per fixture makes each case behave identically alone, in file order, or shuffled.
- The data pointer is 0 because that is what `DriverImpl::open()` registers with the callback, so the driver's registration is restored rather than invented.
- A null mock is tolerated so a fixture that skipped before resolving the mock cannot fault.
- The L2 harness installs the legacy mock on both arms, so `mock->rxCallback` is writable on the AIDL arm and only the caller's check stands between this function and the ill-typed cast in `DriverImpl::DriverReceiveCallback`. `DualPathLegacyFlowTest::SetUp` performs that check first and skips before reaching the call.

### Host observation helpers

- Each insists on the one reply shape the protocol defines for its command and returns a typed value. They return bool rather than asserting so the failure appears at the case's line, and so a case that legitimately expects a refusal can inspect the reply.
- Seven helpers cover the eight commands (`askHostForSessionCount()` serves both session counters, which share a reply shape and purpose): `askHostForSentCount()`, `askHostForSessionCount()`, `askHostForLastSentFrame()`, `askHostForListenerPresence()`, `askHostForRegisteredAddresses()`, `askHostForCallCounts()` and `askHostToDeliverFrame()`. Every one is used by a case: a helper for a command no case sends would imply coverage that does not exist.

### parseOkReplyValue()

- A parser that read the trailing field regardless of the verb would carry a desynchronised channel into an assertion as a plausible-looking number.
- The value is the text after the verb and its single separating space. An empty value is legitimate: `last-sent` answers "OK last-sent " with nothing after it when the fake has captured no frame.
- It fails on an `ERR ` line, a reply for another verb (including a longer word beginning with the verb), or a malformed line, saying which in `failureDetail`.

### askHostForSentCount()

- It is the fake's own application-frame count (allocation polls excluded), read through the fake's own accessor in the host process, which is what makes an outbound assertion mean something.

### askHostForSessionCount()

- The two session counters are the fake's method-level evidence that the middleware's AIDL session lifecycle crossed the binder driver (`sent-count` is the application-frame transmit counterpart, allocation polls excluded): they count the `open()` and `close()` calls the fake's methods served in the host process. `calls` reports the same two methods separately at the transport level, as the `IHdmiCec.open` and `IHdmiCec.close` transactions the fake's `onTransact()` received before dispatch. L1's in-process fake can give neither, because a locally resolved service is called inline; that is why these verbs exist and why cases here consume them.
- They advance at the top of the fake's `open()` and `close()`, before any canned result is consulted.
- Any verb other than the two is refused unsent so a typo reads as a wrong call, not as a host rejecting an unknown command.

### askHostForLastSentFrame()

- Lowercase hexadecimal, two digits per byte and no separators, is the protocol's one payload encoding in both directions. An empty answer (nothing captured) is distinct from a wrong frame, and an assertion has to be able to report the two differently.

### askHostForListenerPresence()

- A `deliver` with no listener held is answered "ERR no-listener" and dispatches nothing, so without this check the negative half of the inbound cases would be vacuous: "no frame arrived" would be satisfied by the trigger doing nothing rather than by the middleware rejecting it.

### askHostForRegisteredAddresses()

- It reads the fake controller's own registration record in the host process, so it reflects the `addLogicalAddresses()` calls that really crossed the binder driver and were accepted, not what the middleware believes it registered.
- The reply is `OK registered <decimal,...>`, the bare `OK registered` when none is registered. Every comma-separated entry must be a non-negative decimal; an empty entry, a sign or trailing text fails the parse, and `addresses` is left untouched.

### askHostForCallCounts()

- The counts are transport-level: the fake's `onTransact()` override counts every incoming transaction by code in the host process before the generated dispatch runs. A remote `getInterfaceVersion()` / `getInterfaceHash()` is answered by the generated `onTransact()` from compiled-in constants and never reaches the fake's virtual methods, and the middleware's allocation polls bypass the fake's application send counter, so only a transport-level count sees every call.
- Consequently `IHdmiCecController.sendMessage` counts every transmit transaction, allocation polls included, and the two metadata methods of each interface are counted. `<interface>.other` sums every code with no named field. The exceptions are the framework transactions `BBinder::transact()` answers before `onTransact()` runs (`PING_TRANSACTION`, `EXTENSION_TRANSACTION`, `DEBUG_PID_TRANSACTION`, `SET_RPC_CLIENT_TRANSACTION`), none of which is an AIDL method.
- The helper insists on exactly the 16 keys the host defines, each once, as `key=decimal` tokens separated by single spaces; an unknown, repeated or missing key, an empty token or a non-decimal value fails the parse, and `counts` is left untouched. The D2 case requires exactly one `IHdmiCec.getLogicalAddresses` across `LibCCEC::getLogicalAddress(1)` with every other count unchanged; the D3 case requires all 16 unchanged across `LibCCEC::getPhysicalAddress()`.

### askHostToDeliverFrame()

- It is the only inbound trigger: the fake fires through its own `fireOnMessageReceived()`, so the listener invoked is exactly the one the middleware handed to `open()` and no second delivery route exists.
- Because `IHdmiCecEventListener` is `oneway`, the reply says only that the callback was invoked; whether the middleware queued, decoded and dispatched the frame is what the case asserts on its own side.
- `hex` carries no separators; `deliveredBytes` must equal half its length. The host refuses with "ERR no-listener" when it holds no listener and "ERR bad-hex" when the payload is malformed.

### toLowercaseHex()

- It serves both directions of one comparison: building a `deliver` payload, and rendering the bytes a case encoded into the string the host's `last-sent` reply must equal. It is written out rather than built on a stream manipulator so the wire format shared with another process is visible where it is produced; a formatting drift would otherwise appear as a byte-comparison failure with no clue why.

### RecordingProcessor

- Per-overload counters, not one "something arrived" flag, make the helper usable on both back-ends without weakening either: a transport that mangled an opcode or dropped an operand would satisfy a boolean but not these counters.
- Only the four message types the file asserts on are overridden; `MessageProcessor`'s default bodies leave every other type uncounted, which the negative assertions rely on.
- Thread safety: the counters are written only by `MessageDecoder::decode`, called from `DecodingFrameListener::notify` while that listener's mutex is held and before its notification counter is published, so a test thread that has returned from `WaitForNotification` has observed a completed decode and cannot read a partially written counter. If that ordering is rearranged, this class needs a lock of its own.
- Constructor: numeric captures start at -1 rather than 0 so "never decoded" differs from "decoded as zero", which matters because 0.0.0.0 is what a zeroed operand pair renders as; the text capture `activeSourcePhysical` starts empty.
- `process(ImageViewOn)`: the message carries no operand, so only the count and header matter.
- `process(ActiveSource)`: the only overload whose message carries operands; they are recorded three ways so an operand pair that survived transit but changed value cannot pass.
- `process(Standby)`: the state-guard case uses this opcode as its post-restore control because it differs from the frame delivered while the driver was closed.
- Counters: a case asserts the expected counter at one and the other three at zero, so a frame that arrived as the wrong type fails instead of satisfying a "something arrived" check.
- `activeSourcePhysical`: `PhysicalAddress::toString()` overrides `CECBytes::toString()` to produce dotted decimal, so this is the human-readable address, not the packed byte pair, which is recorded separately.
- `activeSourceNibbles`: taken from `PhysicalAddress::getByteValue(0..3)`. A non-empty rendered string is satisfied by any address (0.0.0.0, a shifted 0.1.0.0, a truncated value), so the four nibbles pin the value exactly, and asserting each nibble makes a failure name which digit moved.
- `activeSourcePackedHigh`/`Low`: two nibbles per byte. The nibbles and these bytes are two views of the same two octets: the nibbles catch a value that decoded wrongly, the bytes a value that decoded correctly but would not re-encode to the same wire image, which is the property an outbound case on the other side of the same address depends on.
- `lastInitiator`/`lastDestination`: asserted by every inbound case, because a header rewritten in transit is a transport defect the opcode counters cannot see.
- `recordPhysicalAddress()`: serializing into a `CECFrame` is the same public path the encoder uses and the only way a consumer can reach the bytes. Leaving both packed members at -1 for a result other than two bytes reports a malformed operand as unset instead of reading past the end.

### DecodingFrameListener

- Delivery is asynchronous on both arms: the Bus reader thread notifies listeners, never the thread that caused the frame to appear. A fixed sleep is too short under load or inside an emulated guest, wasteful otherwise, and never evidence; a condition variable with `wait_for` is correct and quick, and its expiry is a real verdict, which lets the helper serve the negative control as well.
- The delivering thread is recorded in the listener because the test thread is blocked in `WaitForNotification` while the delivery happens. On the AIDL arm that is half the requirement: invocation E must show a received frame arrived without the test thread delivering it, and comparing `NotifyingThread()` with the test body's `std::this_thread::get_id()` fails if a callback were delivered inline.
- Ordering inside `notify()`: `MessageProcessor` state is not thread safe and `MessageDecoder::decode` mutates it. Publishing the frame, thread id and counter before decoding would let a waiter wake (on the counter or a spurious wake) and read the processor while the decode was still writing it, flaking in the direction of passing on a corrupt read. Everything a waiter can observe is written under the lock with the counter last, so a waiter that sees the counter has seen a completed decode and a spurious wake finds the predicate unsatisfied. Holding the lock across the decode also serialises deliveries, which costs nothing because the Bus reader is a single thread.
- Cost: a waiter that times out concurrently with a delivery blocks on the mutex until that decode finishes — a switch and one `process()` call — and cannot extend the wait beyond one delivery's work.
- Exception containment: `Bus::Reader::run()` calls `notify()` inside a try block that catches only `InvalidStateException`, so any other exception would leave the reader thread's `run()` and terminate the process. `MessageDecoder::decode` already swallows `std::exception` around its opcode switch, so this is a second line. Failures are counted, and every case that expects a delivery asserts `DecodeFailures()` is zero, so a containment that fired is reported rather than hidden.
- `notify()`: the thread id recorded is this thread's on purpose — the thread that delivered the frame, the Bus reader on both arms — which is what makes `NotifyingThread()` meaningful to a test body that was blocked meanwhile.
- `WaitForNotification()`: a predicate wait rather than a sleep, so a negative case waits out a real bound instead of racing the Bus reader, and its expiry is a verdict it can rely on.
- `DecodeFailures()`: a non-zero value means a frame reached the listener and the decoder could not interpret it, a different failure from "no frame arrived" that must read differently in a log; hence its own counter rather than a silent catch.
- `NotifyingThread()`: a default-constructed id compares equal to no running thread, so asserting it differs from the test thread would pass vacuously; every case reading it first establishes that a notification arrived.
- Private state: mutable because `FrameListener::notify()` is const; guarded rather than atomic because the decode and its publication must be one critical section, with `notifications` — the predicate `WaitForNotification` waits on — written last.

## tests/L2Tests/ccec/test_DualPathIntegration.cpp (part 2 of 2)

Detail moved out of the comments from `ScopedConnection` to the end of the file when they were condensed. The code did not change.

- Superseded: none. No moved statement describes logical- or physical-address behaviour. Every `1.0.0.0` in this part is an `<Active Source>` operand, not the driver's physical address.

### ScopedConnection

- It exists because a fatal assertion returns from the test body at once, and `Connection::~Connection()` has an empty body.
- The hazard is concrete. `Connection::open()` registers the connection's own `busFrameListener` with the `Bus`, and only `Connection::close()` removes it. A case that added a stack-allocated `DecodingFrameListener` and then hit an `ASSERT_*` before `close()` would return with the `Bus` still pointing at a listener about to be destroyed and at a still-registered connection. The next frame the reader thread delivers, from a later case or from the same case's in-flight injection, is dispatched through those dead pointers. That is undefined behaviour on the Bus reader thread: it shows up as a crash or corruption in an innocent case, and the first failure's cause is lost.
- A trailing `close()` cannot fix this, because statements after a fatal assertion do not run. A destructor runs on a normal return, on a fatal assertion's early return and when an exception escapes the body.
- `Connection::close()` clears the connection's frame listeners and removes its bus listener, so it is sufficient on its own. `removeFrameListener()` still runs first, so the case's own listener is detached before the connection's teardown touches anything.
- Everything in `release()` is wrapped, because a destructor that throws during unwinding terminates the process and replaces a reported failure with an unexplained abort. A failed cleanup is reported through `ADD_FAILURE()` rather than swallowed, so a close that did not work stays visible.
- The listener ordering: a case declares its listener before the guard, so declaration order unwinds the two in the opposite order.
- Constructor: the connection is always opened, because every case in this tier wants an open connection. A case that wanted a closed one would not need this guard.
- `release()`: a second call does nothing, so an early release and the destructor's release cannot both close.

### ScopedCecLibraryCycle

- It touches process-global state that every other case in the binary shares, so the restoration is a destructor rather than a statement at the end of a body.
- Exactly one case needs it, and that case has no alternative. It must show that a frame delivered while the driver is not OPENED is rejected, and the driver's state belongs to the library, not to a fixture. `LibCCEC::term()` is the only way to leave OPENED, and while the library is down the Bus is stopped and the HAL is closed for the whole process. The L1 async unit (`tests/L1Tests/ccec/test_DriverImpl_Async.cpp`) records the same disposition for the same reason: exactly one case there cycles the shared library.
- A fatal assertion would skip a trailing `init()`, so every later case, and the global environment's own `term()`, would assert against a stack that never came back up.
- Neither half throws; both report. `TakeDown()` and `Restore()` return a bool and a sentence, so the case can attach its own `ASSERT_TRUE` and fail at its own line. The destructor's implicit restore reports through `ADD_FAILURE()`, because a library that could not be re-initialised is a fact the run has to carry, even though nothing can be done about it by then.
- B2 detail: cycling reaches `Driver::close()`. Its AIDL mapping to `IHdmiCec.close` is a high-confidence candidate pending owner confirmation, because `HdmiCecClose` has no mapping-table entry. A green result does not confirm the mapping. The production method carries the same marker.
- Constructor: construction is inert, so a case can declare the guard at the top of its body and cycle the library later.
- `TakeDown()`: on the AIDL back-end, `term()` is a real transaction to the host process, so the host must still be serving. It always is, because the harness reaps the host only in `TearDown`.
- `TakeDown()` failure arm: `LibCCEC::term()` clears its own initialized flag only after `Driver::close()` returns, so a close that raised leaves the library marked initialised. Recording that is what tells `Restore()` not to call `init()` a second time on a library that never came down.
- `down`: set only when `term()` returned.

### DualPathSelectionTest

- It asks nothing of the HAL or the transport, so it has no reason to skip. Its question — which back-end the factory resolved to, and whether that answer is stable — means something under every invocation, and its answer tells a reader whether the other two fixtures did what their names claim.
- It holds the one harness-wide guarantee in this file, `WriteControlCommandReportsEpipeAndTheChildIsStillReapedInsteadOfKillingTheRunner`, because that guarantee has to hold on both arms. The two arm-specific fixtures each skip under the opposite invocation, so a case placed in either would go unchecked on exactly the arm where nothing else was watching. That case is about the harness's signal disposition, control-channel write and child reaping, and touches neither back-end.
- Both casts are computed once in SetUp, because the selection is fixed before any case body runs. Recomputing them per assertion would suggest a volatility that does not exist.
- `dynamic_cast` is the whole mechanism. It is legitimate because `ccec/src/DriverImpl.hpp` and `ccec/src/DriverAidlImpl.hpp` are not installed headers. No production introspection API is added, and none may be.
- TearDown: the fixture takes no lock, installs no callback, sets no mock expectation and opens no Connection.

### DualPathLegacyFlowTest

- The route is installed in the fixture's SetUp rather than in each case. That is safe for the outbound cases too, because it only restores the registration `DriverImpl::open()` itself installed.
- Why the SetUp order matters: the inbound cases reach the stack through `DriverImpl::DriverReceiveCallback`, which finds its target with `static_cast<DriverImpl &>(Driver::getInstance())`. That cast is ill-typed, and so undefined behaviour, whenever the factory resolved to the AIDL back-end. `tests/L2Tests/test_main.cpp` installs this same legacy mock on both arms, so `mock->rxCallback` is writable under invocation E too. The back-end check therefore runs before the route is installed, and the installation cannot be reached when the check skips.
- SetUp step (1): the global environment, which runs before any fixture, creates the mock. If it is missing, the harness has failed; that is not a condition to work around.
- SetUp step (3): this is not a convenience check, and it cannot swap places with step (4). `GTEST_SKIP` returns from SetUp, so step (4) is truly unreachable on the AIDL arm, not merely discouraged.

### DualPathAidlFlowTest

- Several conditions must hold before the AIDL back-end can be selected. The fake service host must have launched and reported ready before `LibCCEC::init` ran, which needs `CEC_TEST_AIDL_MODE=remote` and a binder-capable kernel with a running servicemanager. None of this can be arranged from a test body, so the fixture skips instead of failing when the arm is not its own. Skipping keeps one registered case count valid for invocations D and E, which lets the coverage runner gate both on a single expected number.
- Why the mock is off limits: `tests/L2Tests/test_main.cpp` installs the legacy mock on both arms unconditionally, so every forbidden use compiles and can be reached. An expectation or an injected transmit result does nothing, because the AIDL back-end never calls the legacy C entry points, so a case built on one would assert against a mock nobody drives. Writing `rxCallback` arms the undefined-behaviour cast described under `DualPathLegacyFlowTest`. The AIDL arm's stimulus comes from the host process and nowhere else.
- SetUp treats its two checks differently. A wrong arm is a skip, because the cases then have nothing to test. A missing channel is a failure, because the selected arm implies the channel exists.
- SetUp step (1): skipping is the honest report, and adapting the cases would be a fiction.
- SetUp step (2): the harness always gives the host a control and observation channel, and pings the host over it before initializing. A closed channel here contradicts the selected arm, which points to a defect in the harness or the host, not to a platform this tier cannot run on. Skipping would let invocation E report green while none of the observations it exists to make had been made.

### Selection (section banner)

- The section holds the four `DualPathSelectionTest` cases, and all four run under every invocation. Three ask which back-end resolved, whether it is stable and whether it is the one requested; the fourth is the one harness guarantee that must hold on both arms.

### DualPathSelectionTest.TheFactoryResolvedToExactlyOneBackEnd

- It establishes that the two back-ends are independent siblings of the `Driver` interface, selected between rather than stacked in layers. That is the migration's central property.
- The hierarchies a one-sided check would miss are a single class inheriting from both implementations, or one implementation made a base of the other. Asserting that the other cast fails rules them out.
- It also establishes the precondition the two arm-specific fixtures rely on: exactly one of them runs its cases and the other skips, never both and never neither.

### DualPathSelectionTest.RepeatedGetInstanceCallsReturnTheSameObject

- It checks, at the L2 tier, the requirement that the selection resolves once at initialization and holds for the life of the process. It does so over a real process lifetime rather than inside a unit fixture.
- By the time the body runs, the library is initialized, the driver is open, the Bus threads are running, and production code has called the factory an unknown number of times. If the factory ever re-resolved, because a service appeared or the static initializer ran again, it would show here.
- No service is registered or deregistered. The fake is not linked into this runner, and the pinned C++ `IServiceManager` has no service-removal API, so a case built on unregistering could not be written even if the fake were linked. Object identity is the stability that can honestly be asserted.

### DualPathSelectionTest.TheResolvedBackEndMatchesTheModeTheHarnessWasGiven

- Without it, a misconfigured invocation that quietly ran the legacy path would report green and be filed as AIDL evidence.
- It is the only case in the file that consults the requested mode, and it obtains it through `cecL2RequestedAidlMode()` rather than reading `CEC_TEST_AIDL_MODE`, which only the two `test_main.cpp` files read. The selection is made before `LibCCEC::init`, so nothing a test body did afterwards could change it.
- Only three states can be seen here: `absent`, `remote`, or unset. The harness rejects `compatible` and `incompatible` with a hard failure naming `run_L1Tests`, and rejects any unrecognised value with a hard failure too.
- An unset or empty value skips rather than assuming a default. The harness does document unset as meaning absent, and a bare `./run_L2Tests` is a legitimate way to run the legacy arm. But this case asserts that an outcome matches a request, and with no request there is nothing to match. Skipping costs the invocation matrix nothing, because the coverage runner sets the variable explicitly for both D and E.

### DualPathSelectionTest.WriteControlCommandReportsEpipeAndTheChildIsStillReapedInsteadOfKillingTheRunner

- The property: the harness writes commands to a pipe read by the fake service host, a separate process that can exit or close its read end at any moment. Under SIGPIPE's default disposition that write kills the runner, and two things the tier promises die with it. First, the EPIPE arm of `writeControlCommand()` never runs, so the failure is recorded nowhere and the run looks like a crash of unknown origin. Second, the global environment's TearDown never runs, so the host is neither signalled nor reaped. It outlives the run holding the production service name, and the next run fails on a stale registration. `tests/L2Tests/test_main.cpp` therefore installs `SIG_IGN` as the first step of `CecL2TestEnvironment::SetUp` and restores the previous disposition in TearDown. This case catches the removal of that install and proves that both things the install provides are really there.
- It drives the real code. Both promises are about code that runs only after a pipe's reader is gone, and no ordinary invocation reaches that state, because a healthy host reads its control descriptor until teardown closes it. A look-alike case, with its own pipe, its own raw `::write()` and its own errno, would only show kernel behaviour and the process's signal disposition, neither of which is in doubt, and would leave `writeControlCommand()`'s diagnostic and the reap untested. So the seam `cecL2ProveEpipeDiagnosticAndChildReaping()` calls the real `writeControlCommand()` against a descriptor whose reader has truly gone, requires the command-specific EPIPE sentence back, and then ends a real child through the same `terminateAndReapChildProcess()` a teardown uses. Neither is a copy, so a change that broke either breaks this case.
- It is deterministic on any host. The seam builds the hazard from two pipes and a child of its own. Closing a pipe's last read end and then writing to its write end returns -1 with EPIPE immediately, and SIGTERM ends a child whose disposition is `SIG_DFL`. Nothing sleeps and no wall clock is polled; each wait is a real wait on a real event, under one bound. So it behaves the same under invocation D on a host without binder support, which is where it usually runs, and under invocation E in the binder-capable guest. It uses only its own resources, so it cannot disturb the harness's live channel or the host's descriptors, and it leaves no descriptor and no child behind on any path.
- Step (1) comes first and is fatal. If the disposition were the default, the writes in the later steps would kill the runner mid-case, and a suite cannot report on what killed it. It asserts "not the default" rather than "exactly `SIG_IGN`": the requirement is only that a broken pipe does not kill the process, which a handler satisfies as well as `SIG_IGN` does, and pinning the exact value would fail a future harness that met the requirement another way. Passing a null action pointer makes `sigaction` a query, so the case observes the harness's choice. A case that installed the disposition itself would pass whether or not the harness had.
- Step (2): the seam's own report is asserted first. The diagnostic it returns is then asserted at this case's line for the two things that give it value: it names the lost command, and it reports EPIPE rather than an expired bound. The deadline arm of the same function also names the command, so without EPIPE the failure could be a timeout, which is a different failure entirely. This step turns the SIGPIPE install's two purposes, a diagnostic that is produced and a reap that is performed, into facts about this binary rather than claims. The command text `epipe-probe` is deliberately spelled again in this file, as a two-place contract like the `extern` declarations; if the seam ever sends a different command, this assertion fails and both places are updated together.
- Step (3) adds the one observation step (2) makes only indirectly: `write()` returns, returns -1, and sets EPIPE. It is kept as the cheapest possible statement of the mechanism the seam relies on, and labelled secondary because on its own it would prove nothing about the harness. The pipe uses `O_CLOEXEC` because every descriptor this tier creates does; nothing here forks, but a descriptor that survives an exec for no reason tends to become a bug later.
- Reaching the assertions at all is the rest of the evidence, and it cannot be written as an assertion, because a process that had died would report nothing.

### DualPathLegacyFlowTest.InboundImageViewOnReachesTheTypedProcessorThroughTheLegacyBackEnd

- It establishes the middleware leg of flow A on the legacy back-end. It requires invocation D with `DriverImpl` resolved, which the fixture confirms before installing the inbound route.
- The evidence is the recording processor's typed counters and header nibbles, read after the listener has confirmed that a notification arrived. The destination nibble 0 matches the connection, so Connection's filter must let the frame through.
- It asserts the whole middleware leg: not merely that a frame reached a listener, but that it arrived still decodable as `<Image View On>` with its header nibbles intact.

### DualPathLegacyFlowTest.InboundBroadcastActiveSourceCarriesItsOperandsThroughTheLegacyBackEnd

- It establishes that operand bytes survive the inbound leg exactly, not merely that something arrived. Both operand bytes have to survive the heap copy, the queue, the reader thread, the filter and the decoder.
- A non-empty check would accept 0.0.0.0 from a zeroed operand pair, 0.1.0.0 from a nibble shift, or a truncated value. Asserting the exact value makes this case a check on the transport, not just on the fact that something was copied.
- `PhysicalAddress::toString()` renders dotted decimal and `PhysicalAddress::getByteValue(0..3)` yields the four nibbles, so the exact value is available and there is no reason to settle for less.
- It deliberately establishes nothing about the AIDL arm. The stimulus is the legacy HAL mock's Rx callback, so this is evidence for invocation D only. The AIDL inbound cases carry their own evidence, and neither substitutes for the other.

### DualPathLegacyFlowTest.InboundFrameForAnotherAddressIsFilteredBeforeDecodingOnTheLegacyBackEnd

- It establishes that Connection's address filter runs before the decoder. The evidence is a wait that is expected to expire, plus a notification count and every typed counter still at zero.
- Without it, a listener that received every frame regardless of address would pass both positive inbound cases. Destination 3 is Tuner 1.
- The wait runs to its full bound rather than checking at once. Otherwise the case would only prove that the test thread was faster than the Bus reader thread.

### DualPathLegacyFlowTest.OutboundImageViewOnReachesTheLegacyHalAsExactBytes

- It establishes the middleware leg of flow B on the legacy back-end. The evidence is the buffer and length captured by the mock's `HdmiCecTx` expectation, compared byte for byte with the wire image.
- `Connection::sendTo` calls into `Bus` and then `DriverImpl::write` on the calling thread. The wire image is the header nibbles followed by the opcode, nothing more and nothing less. Initiator 4 is Playback Device 1.

### DualPathLegacyFlowTest.OutboundActiveSourceReachesTheLegacyHalWithOperandsInWireOrder

- It establishes that a multi-byte frame's operands reach the HAL in order and unchanged. All four captured bytes are asserted individually, not just the length. The physical address 1.0.0.0 is packed two digits per byte.

### DualPathLegacyFlowTest.OutboundTransmitFailureFromTheLegacyHalSurfacesAsAnException

- It establishes the error leg of flow B on the legacy back-end. `HDMI_CEC_IO_SENT_FAILED` arrives through the result out-parameter.

### Flow B on the AIDL back-end (section banner)

- `Connection::sendTo -> Bus -> DriverAidlImpl::write -> IHdmiCecController::sendMessage` crosses the driver into the host process and returns a real `SendMessageStatus`. Completing that round trip is part of the evidence, because a real `Bp*` proxy, a real transaction and a real reply are things no in-process fake can produce. It is not the whole of the evidence.
- "It did not throw" is not enough on its own. A send that reached the service with corrupt bytes, a fake that dropped it, and a marshalling step that wrote an empty vector all complete a transaction and return a status. The fake records the bytes it received in the host process, and it is not linked into this runner, so the only way to read them is to ask the host.
- Each case reads `sent-count` before and after over the pipe channel and asserts it advanced by exactly one. "Moved" is not enough, because a retry loop that sent the frame twice is a defect and would still satisfy "greater than before". Each case also reads `last-sent` and asserts it equals the exact lowercase hex of the bytes the case encoded.
- The observation travels over the pipe rather than over binder, because binder is what is under test. Asking the service over binder how a binder transmit went would use the transport to vouch for itself, and a fault could corrupt the evidence without anyone seeing it. The pipes behave the same whether the driver is healthy, degraded or absent.

### DualPathAidlFlowTest.OutboundImageViewOnCrossesRealBinderIpcToTheFakeService

- It establishes the middleware leg of flow B on the AIDL back-end, over a real proxy and a real driver transaction. It requires invocation E with `DriverAidlImpl` resolved and the fake service host serving with its control channel open.
- The bytes: initiator 4 (Playback Device 1, the connection's source), destination 0 (TV), opcode 0x04. The message is directed, which matters for the status translation.
- (1) The round trip completed. The throwing `sendTo` overload is used deliberately, because with the non-throwing one a failed transmit looks the same as a successful one. That makes `EXPECT_NO_THROW` a real assertion about the whole stack rather than a tautology. A `CECNoAckException` would mean the status translation read the reply as a rejection; an `IOException` would mean the binder `Status` came back not-ok or the frame failed the length guard. The check relies on the hosted fake's documented default reply, `ACK_STATE_0`, which this runner cannot change because the fake is not linked into it. That reply is correct for this frame, because `ACK_STATE_0` on a directed message means acknowledged.
- (2) The frame arrived exactly once. The check is "exactly", not "at least", because on a real bus a duplicate `<Image View On>` is a second command to a real device.
- (3) The frame arrived intact. The expected value is rebuilt with the same two calls `sendTo` makes (`Header(...).serialize`, then `append`) rather than written as a literal, so it also catches the middleware writing the right length with the wrong content. The literal `4004` that CEC defines is then checked against the rebuilt value, the same way the legacy arm of this flow asserts.
- The payload is pinned separately. `MessageEncoder` writes only the payload, the opcode and its operands, into the caller's frame. `Connection::sendTo` builds its own local `CECFrame`, serializes the header into it and appends the caller's frame (`ccec/src/Connection.cpp:141-157` when this was written). The caller's frame is taken by const reference and never modified, so the frame the case holds is one byte shorter than the wire image. Asserting the wire length against the caller's frame would amount to asserting that `sendTo` mutates its const argument. Pinning the payload means the case cannot agree with a corrupt send by having encoded the message wrongly itself. Because the expected wire image is derived rather than hardcoded, the case also cannot agree with a wrong header by restating one.
- It deliberately establishes nothing about the inbound direction, which the two inbound cases own.

### DualPathAidlFlowTest.OutboundActiveSourceWithOperandsCrossesRealBinderIpc

- It establishes that the length and operands survive the parcel on the AIDL back-end, and that the broadcast arm of the status translation does not raise for this opcode. The evidence is the fake's captured frame and application-frame count (allocation polls excluded), both read over the pipe channel.
- `DriverAidlImpl::write` copies the frame into a `std::vector<uint8_t>` for `sendMessage`. A four-byte frame with operands therefore has to be marshalled, written into the parcel, read back on the far side and accepted. A two-byte frame would miss a length error, an off-by-one in the copy, or an operand lost in marshalling. The case also exercises the length guard from the compliant side: four bytes is well inside the 16-byte AIDL contract, so the guard must let the frame through.
- For a broadcast, `ACK_STATE_0` means rejected rather than acknowledged; the meaning inverts. The middleware raises `CECNoAckException` on that arm only for the CEC CTS 9-3-3 case, a rejected `<Report Physical Address>`. This frame's opcode is 0x82, so that rule does not apply and the send returns normally against the host's default reply.
- The operands are asserted at the service, so an operand dropped, reordered or zeroed inside the parcel fails the case. The encoder's payload is pinned separately, because `Connection::sendTo` prepends the header to a local copy. The count check tells "the bytes were wrong" apart from "the transmit never arrived"; these are different defects and must not share a failure message.
- It deliberately leaves out the refusing side of the 16-byte length guard. The boundary at 16, 17 and 20 bytes belongs to the L1 contract suite, where the fake's canned status can vary per case.

### Flow A on the AIDL back-end (section banner)

- Both cases run, using the host's control channel. Only the fake service can invoke the middleware's `IHdmiCecEventListener`, and by design the fake lives in the host process, so this runner cannot call it directly. Instead it sends `deliver <hex>`, which reaches the fake's own `fireOnMessageReceived()`. That invokes the listener the middleware passed to `open()`, and no other, so the delivery route under test is the production route, not one invented for the test.
- These are the only cases in the repository that can prove inbound AIDL delivery. `onMessageReceived` is `oneway`, so it is dispatched inside this process on a binder threadpool thread: the pool `DriverAidlImpl::open()` starts, which is why it has to start one. No in-process fake can reproduce this, because a locally registered service resolves to the local `BBinder` and its callback runs inline on the calling thread.
- What the thread assertion shows: the listener records the thread that delivered its notification, which on both arms is the Bus reader thread, not the thread that produced the frame, because the queue does the handoff. So `NotifyingThread() != this_thread` is a necessary condition: it fails if delivery ran inline on the test thread. That the callback itself ran on a binder thread follows from the structure. The frame can only come from the fake, the fake is in another process, its only route into this process is the binder driver, and only the client threadpool's threads execute an incoming oneway transaction here.

### DualPathAidlFlowTest.InboundFrameFromTheFakeServiceArrivesOnABinderThreadAndReachesTheTypedProcessor

- It proves the one thing invocation E can prove and no in-process fake can: a frame the service originates crosses the binder driver into this process and is dispatched by the client threadpool to the middleware's `IHdmiCecEventListener`. From there it travels through the same receive queue, Bus reader thread and address filter as a legacy frame, and reaches the same typed `process()` overload.
- The Connection is opened as TV, so the destination matches and the filter must let the frame through. The frame deliberately matches the legacy inbound case's, so the two arms see identical input and any difference in result comes from the back-end.
- (1) The trigger was real. `listener` is checked first, because a `deliver` with no listener held is answered `ERR no-listener` and dispatches nothing; without this check, a middleware that never registered its listener would fail on the wait with a misleading message. The `deliver` reply then reports how many bytes the fake handed to the callback, and two are asserted, so a truncated trigger is caught before the wait.
- (1b) The session behind the listener is real, and there is only one. `open-count` and `close-count` are the fake service's own method-level counters, read in the host process, so they show that the session lifecycle crossed the driver, not just a transmit. `calls` reports the same two methods at the transport level as `IHdmiCec.open` and `IHdmiCec.close` transaction counts; this case reads the method-level pair. In L1, the in-process fake resolves locally and is called inline, so a count there proves nothing about a transaction. The case asserts exactly one more open than close, meaning one live session, and re-reads both counters at the end, where they must be unchanged. The check is a difference rather than the literals 1 and 0. If no case has cycled the library the counters are 1 and 0, but the state-guard case cycles it deliberately, adding one to each counter per cycle. GoogleTest runs cases in registration order by default, but this file must pass under `--gtest_shuffle`, so a literal would really be an assertion about case order. The middleware opens exactly once, in `LibCCEC::init`, and never again.
- (2) The frame arrived within a bound. This is a predicate wait, not a sleep, so its expiry is a real verdict that the frame never arrived. The bound covers an inter-process oneway transaction, the queue handoff and the Bus reader's wake-up, inside an emulated guest on a loaded machine.
- (3) It arrived as the right message. `imageViewOnCount == 1` with the other three overload counters at zero, so a frame decoded as a different message fails. Both header nibbles are asserted, so a rewritten initiator or destination fails. `DecodeFailures() == 0`, so a delivery whose decode threw and was contained is reported, not hidden.
- (4) It was not delivered inline. Checking `NotifyingThread()` against this thread's id is the necessary condition; the binder-thread half follows from the structure, as described under the Flow A banner above.
- (5) The session survived the delivery unchanged. This is the second half of (1b), asserted after the frame arrives, so a back-end that reopened its session for each inbound frame, or closed and reopened it around the callback, fails here instead of passing every check above. It is an exact expectation rather than an invariant, because it covers only this case.
- It deliberately does not assert the address filter's negative side: the legacy filtered case covers it, and the filter is shared code above the seam. Close-state rejection belongs to the next case.

### DualPathAidlFlowTest.AFrameDeliveredWhileTheDriverIsNotOpenedIsRejectedByTheStateGuard

- It establishes that the AIDL listener's state guard refuses a frame outside OPENED, just as the legacy delete-on-throw path does. The evidence is the fake's delivery reply, the typed counters after the library comes back up, a second frame with a different opcode that must arrive, and the fake's open and close counters read at each transition.
- The legacy path: `DriverImpl::DriverReceiveCallback` does not touch the queue directly. It offers through `DriverImpl::getIncomingQueue()`, which raises `InvalidStateException` when the status is not OPENED, and the callback then deletes the frame. The AIDL listener must reject the same way. Offering straight to the queue would accept frames the legacy path refuses, and a frame accepted while closed reaches the application after its session has ended.
- The hard part is observing the rejection. A `oneway` callback has no caller to receive a fault, so the fake cannot tell whether the middleware accepted or rejected the frame; its reply says only that the callback was invoked. The observation has to be made on this side, in four steps that together rule out a vacuous pass:
  - (1) The delivery really happened while the driver was out of OPENED. The fake deliberately keeps the listener across close, as its contract states, precisely so this is testable. `deliver` therefore reports the callback invoked with two bytes, not `ERR no-listener`. A negative that relied on the trigger doing nothing would prove nothing.
  - (2) The frame never surfaces, even after the stack comes back up. This is what tells "released" apart from "queued". A frame wrongly offered while closed would sit in the receive queue. `DriverAidlImpl::read()` consumes a NULL sentinel and loops rather than draining the queue behind it, so the Bus reader started by re-initialisation would deliver that frame. That is why the listener stays attached across the whole cycle. The restore happens before the negative check, because a closed stack has no Bus reader and would show "no frame arrived" whether the frame was released or queued.
  - (3) The route is alive afterwards. A second frame, `<Standby>` (`4036`), is delivered after the restore and must arrive. Without it, step (2) would pass on a route that had simply stopped working. The different opcode makes the identification exact: one notification that decodes to `<Standby>` means the closed-window `<Image View On>` was released, while an `imageViewOnCount` above zero at the end means it was queued and delivered late.
  - (4) The cycle reached the service once in each direction. `open-count` and `close-count` are read before the take-down, after it, and after the restore, and each transition must move exactly one counter by exactly one. This turns "term() did not raise" into "the close crossed the binder driver". It would fail if the take-down closed nothing on the far side, in which case step (2) would pass for the wrong reason. It is also the only place where a session transition, as opposed to the standing one-live-session invariant the inbound case checks, is observed from outside the process that owns it. A close issued twice would fail on the real HAL. A take-down that reopened anything would leave the state guard nothing to reject. A re-initialisation that opened nothing would leave the control delivery reaching a stale listener.
- The counters are read before the cycle rather than assumed to be 1 and 0, because under `--gtest_shuffle` another case may run first. What is asserted is a pair of deltas.
- Destruction runs in reverse declaration order: the library is restored, then the connection is closed against a stack that is up, and finally the listener is destroyed with nothing pointing at it.
- B2: step (4) observes that the close call was made, once. It says nothing about whether `IHdmiCec.close` is the right mapping for `HdmiCecClose`, and a green result does not confirm that mapping.
