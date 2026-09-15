# HDMI CEC Mocks

This directory contains mock implementations of external dependencies for HDMI CEC L1 testing.

## Structure

```
mocks/
├── hdmicec/                    # HDMI CEC specific mocks
│   ├── hdmi_cec_driver.h       # HAL driver interface
│   ├── hdmi_cec_driver_mock.h  # Mock class header
│   ├── hdmi_cec_driver_mock.cpp # Mock implementation
│   ├── fake_hdmi_cec_aidl_service.h # Fake AIDL service header
│   ├── fake_hdmi_cec_aidl_service.cpp # Fake AIDL service implementation
│   └── fake_hdmi_cec_aidl_service_host.cpp # Out-of-process fake host binary
├── safec_lib.h                 # safec API compatibility shim
├── telemetry_busmessage_sender.h # Telemetry stub
└── README.md
```

## Files

### hdmicec/hdmi_cec_driver.h
Header file defining the HDMI CEC HAL driver interface. This matches the interface expected by the CEC implementation.

### hdmicec/hdmi_cec_driver_mock.h / hdmi_cec_driver_mock.cpp
Mock implementation of the HDMI CEC driver. This provides:
- Controllable driver behavior for testing
- Methods to inject received CEC messages
- Methods to simulate transmission results
- Verification of driver state and logical addresses

### hdmicec/fake_hdmi_cec_aidl_service.h / fake_hdmi_cec_aidl_service.cpp
Test-scope fake implementation of the existing HDMI CEC AIDL service. The header declares the fake service and controller classes, derived from the generated BnHdmiCec and BnHdmiCecController bases; the implementation supplies their behaviour and registers the fake under the production service name "HdmiCec", which it takes from IHdmiCec::serviceName() rather than from a literal of its own, so the middleware's own lookup resolves to it. It implements no new interface: the interface it implements is the one the .aidl files already define. This provides:
- Settable canned responses for the logical address, transmit and open/close calls, so tests can drive each outcome
- Reporting of a multi-entry or an empty logical address vector
- Each SendMessageStatus value, and a settable non-ok binder Status for every call the middleware actually makes: open, close, getLogicalAddresses, addLogicalAddresses, removeLogicalAddresses and sendMessage
- A call counter for each of those calls, and a last-argument capture for each one that carries an argument worth asserting: the listener passed to open, the controller passed to close, the address vector passed to addLogicalAddresses and to removeLogicalAddresses, and the frame passed to sendMessage. The four calls the middleware deliberately never makes (getState, getProperty, registerEventListener and unregisterEventListener) carry a counter only: they always report an ok status and have no settable response, because the one thing a test wants from them is the assertion that their count stayed at zero
- Three triggers that make the fake call back into the listener captured by open(): a received message, a state change and a message-sent report. All three are called in process by the L1 AIDL suite, the last two by DriverAidlSessionTest.DiagnosticCallbacksAreReportedWithoutDisturbingTheSession; only the received-message trigger is additionally reachable from another process, through the host's control channel described below
- Four metadata controls: setInterfaceHash and setInterfaceVersion on each class, so both interface-metadata values each fake reports are overridable, which is what the fake is required to offer. They are not interchangeable, and what each one can decide differs:
  - setInterfaceHash on the service class is the only one that can change a selection outcome, because the middleware's compatibility check reads the service interface's metadata alone. Its consumer for that purpose is the L1 harness in tests/L1Tests/test_main.cpp, whose incompatible mode installs the broken hash "-1" before the middleware's selection resolves - before, because the selection resolves once per process, so a hash installed afterwards would leave the resolved back-end untouched and produce a green run that proved nothing. That ordering is what makes the factory-level fallback from a present but incompatible service observable.
  - setInterfaceVersion on the service class, and both controls on the controller class, change no selection outcome. The compatibility check never reads the controller's metadata, and the version arms of that check are not driven through this fake at all. What these three do is make the value each fake reports observable and changeable, which is what turns each getter's divergence trace - the line a getter prints when the value it reports differs from its own compiled-in constant - into a branch a test reaches rather than a defensive one nothing can drive.
  - reset() on each class restores every metadata value that class can change, alongside its canned responses, so an override cannot outlive the case that installed it. This matters more than tidiness: the harness registers one fake for the whole process, again because the selection resolves once, so a value that survived a reset would decide the outcome of every later case.
- The compatibility-rejection arms this fake does not reach - the empty hash, the "notfrozen" development hash and every version arm - are exercised at unit level by locally constructed doubles in tests/L1Tests/ccec/test_DriverAidl.cpp rather than through the fake, because a Bn*-derived object that is served remotely cannot report divergent metadata at all: its generated onTransact answers the metadata transactions from the compiled-in constants. The same reason bounds all four controls above to in-process dispatch, and it is why they must not be judged redundant on the evidence of a remote run ignoring them. The four cases in DriverAidlCompatibilityTest whose names begin TheServiceFake and TheControllerFake drive the controls directly, on fakes constructed locally and never registered - registration would call defaultServiceManager(), which opens the binder driver and aborts where no kernel binder support exists

### hdmicec/fake_hdmi_cec_aidl_service_host.cpp
A small main() that hosts the fake in a separate process. A service name registered in the calling process resolves to the local BBinder, so an in-process registration produces no proxy and no transaction across the binder driver; only a separate process makes the middleware hold a real proxy and receive callbacks on a binder thread. It starts a binder threadpool for its own service side, signals readiness to its parent, and exits on request. The test harness launches it, waits for that readiness signal, and terminates and reaps it on teardown.

#### How the harness launches it, and what that guarantees about processes and descriptors

Two properties of the launch belong here rather than only in the harness, because the host is on the other side of both and because a host binary written later has to be able to rely on them.

- **The host is a process group leader, and teardown is group-wide.** The harness calls setpgid() on both sides of the fork - in the child before it execs, and in the parent after it - and then reads the group back with getpgid(), signalling the group only when it equals the child's pid and differs from the runner's own. What that buys is the only instrument that reaches a process the HOST started: waitpid() collects one process, so anything the host forks is beyond it, and before the group existed such a process survived the run reparented to init while the harness reported a clean teardown. Teardown now sends SIGTERM to the group, waits for the host within its bound, escalates SIGKILL to the group, reaps the host, and then requires kill() to the group to fail with ESRCH before it reports success. The read-back is not ceremony: every process starts in its parent's group, so an unconfirmed group id is the runner's own and signalling it would end the runner.
- **The host inherits the three descriptors it is told about and nothing else.** CEC_FAKE_HOST_READY_FD, CEC_FAKE_HOST_CONTROL_FD and CEC_FAKE_HOST_OBSERVE_FD have O_CLOEXEC cleared for them specifically; every other descriptor above the standard streams is closed in the child immediately before the exec. A fork duplicates everything the parent held - GoogleTest's output file, the binder driver node on an AIDL invocation, whatever an earlier probe still had open - and a host that inherited one would pass it to everything it started; a descendant holding a copy of a pipe's write end is exactly what stops the readiness pipe ever reporting end of file, which is the signal teardown reads as "the host has gone". Standard output and standard error are inherited deliberately, so the host's trace lands in the runner's log.

Both are driven by DualPathHostLifecycleTest in tests/L2Tests/test_main.cpp rather than asserted about here: it forks a child that leads its own group, gives it a grandchild that ignores SIGTERM, hands the child a descriptor it is never told about, and requires the descriptor to have been closed and the grandchild to be gone - the second observed as end of file on a pipe the grandchild was the last writer of, plus the ESRCH probe.

#### Diagnostics naming a value from outside the process

Every diagnostic in this host that names a value it did not choose - the three descriptor variables, a control-channel command and the reply it produced - renders that value rather than printing it: a literal backslash becomes \\, a newline \n, a carriage return \r, a tab \t, any other byte outside printable ASCII \xNN, and the whole thing is cut to 200 rendered characters with "...[truncated, N bytes total]" appended. The value is always delimited, so an empty one is visible as such, and it never begins a line - the host's own [FakeHdmiCecAidlHost] prefix is always in front of it. Together those two make a forged standalone ::error:: line unreachable by construction, which matters because this host's output is inherited by a runner whose log a CI job parses for workflow commands. One contract, five deliberately identical copies: this host is one of them, and the other four are in tests/L1Tests/test_main.cpp, tests/L2Tests/test_main.cpp, tests/L1Tests/run_coverage.sh and .github/workflows/aidl-path-tests-rootfs.sh. Each copy names the other four in its own comment, because five copies that drift are worse than one that is shared, and the file list this work was allowed to touch ruled out a shared header.

#### The control and observation channel

A separate process is opaque to the test that launched it: the parent cannot reach into it to make the fake deliver an inbound message, and it cannot read back what the middleware actually sent. The host therefore carries a line-oriented control and observation channel that does not travel over binder, which is the point of it - the observation stays trustworthy even when the binder path under test is the thing that is broken. The channel is optional and off by default. The host reads two environment variables alongside the existing CEC_FAKE_HOST_READY_FD, and with both unset it behaves exactly as it did before the channel existed:

- CEC_FAKE_HOST_CONTROL_FD: an inherited read end, from which the host reads newline-terminated ASCII commands
- CEC_FAKE_HOST_OBSERVE_FD: an inherited write end, to which the host writes exactly one newline-terminated reply per command, every reply beginning either "OK " or "ERR "

The command vocabulary:

```
ping                             -> OK pong
deliver <lowercase-hex>          -> OK delivered <byteCount> | ERR no-listener | ERR bad-hex
sent-count                       -> OK sent-count <n>
last-sent                        -> OK last-sent <lowercase-hex>   (empty capture: empty hex field)
open-count                       -> OK open-count <n>      (consumed by DualPathAidlFlowTest)
close-count                      -> OK close-count <n>     (consumed by DualPathAidlFlowTest)
listener                         -> OK listener present | OK listener absent
shutdown                         -> OK shutdown, then the same teardown the signal path performs
anything else                    -> ERR unknown-command <verb>
```

That table is the whole vocabulary: every verb that is not in it is answered "ERR unknown-command <verb>" rather than silently served, and the session continues. Three verbs are deliberately absent. state-changed and message-sent would have fired the fake's two diagnostic callbacks over IPC, and the adapter's only obligation for those callbacks - log them and act on them in no other way - is already asserted in process by DriverAidlSessionTest.DiagnosticCallbacksAreReportedWithoutDisturbingTheSession, which calls the two triggers directly and reads what they logged, so an out-of-process variant would have added a verb without adding evidence. reset is absent because it cannot be made safe: FakeHdmiCecService::reset() clears the captured listener, so a reset issued mid-session would destroy the live session's receive path and turn every later deliver into "ERR no-listener" for a reason no assertion would explain. Per-case isolation belongs to the in-process fixtures, which call reset() directly and re-open afterwards.

Three further replies exist outside the table above. A command carrying the wrong number of arguments is answered "ERR bad-args <verb>". A line longer than the 4096-byte cap is answered "ERR command-too-long" and is never parsed, so how the parent's bytes happened to be split can never decide whether its command was accepted. The two commands that read through the controller answer "ERR no-controller" if the fake is somehow holding none, which the host's own construction order makes unreachable today but which is answered rather than crashed. A blank or whitespace-only line is ignored without a reply, a trailing carriage return is tolerated, and the loop continues after every ERR, so a malformed command never costs the parent its channel.

deliver reaches the same listener the middleware handed to IHdmiCec::open(), and the observation commands report the fake's own counters and captured bytes rather than a second copy of them, so what the channel answers is what the middleware actually did. Nothing in the vocabulary clears, rewinds or reconfigures the hosted fake.

Failures are loud rather than silent. One variable present without the other, a value that is not a plain non-negative descriptor number, a descriptor that is not open in the child or is open in the wrong direction, and both variables naming the same descriptor are all hard failures: the host exits 8 and writes no readiness token, so a parent that mis-wires the channel sees a failed launch instead of a host that quietly ignores its commands. A channel that breaks while in use exits 9: a poll or read error, a reply that was not delivered within its five-second deadline, or a reply too long to deliver as one line. Both reply limits belong to the host's own write path rather than to the descriptor its parent handed it. The five seconds are one absolute deadline per reply, taken from the monotonic clock as the write begins, and every wait inside that write gets only the time still remaining, so an interrupted or partially accepted reply cannot extend it; for the duration of the write the descriptor is nonblocking, with its original flags restored on every exit, so an undeliverable reply is abandoned at the deadline rather than blocking the host in write(). The length limit is PIPE_BUF - 4096 bytes on Linux, terminator included - because that is the size up to which a pipe write is atomic, so a reply arrives whole or not at all; a longer line is refused unwritten rather than fragmented, and the only reply this vocabulary can produce that reaches that size is the "ERR unknown-command <verb>" echo of a verb from a client that has lost its framing. The parent must clear FD_CLOEXEC on the child's copies of both descriptors between fork() and exec(), exactly as it already does for the readiness descriptor. The host serves the channel with a poll() over both the control descriptor and its shutdown self-pipe, so a signal and a queued command are each answered promptly, and end of file on the control descriptor means the parent has gone away and is treated as a clean shutdown.

### telemetry_busmessage_sender.h
Stub header for RDK telemetry system. Provides no-op macros for telemetry calls used in the CEC code.

## Usage in Tests

```cpp
#include "hdmi_cec_driver_mock.h"

TEST_F(YourTestFixture, TestSomething) {
    // Create mock instance
    HdmiCecDriverMock mock;

    // Configure mock behavior using Google Mock, for example:
    // ON_CALL(mock, open(::testing::_)).WillByDefault(::testing::Return(true));
    // EXPECT_CALL(mock, close()).Times(1);

    // Your test code that uses the CEC driver
    // ...

    // Inject a received message into the mock
    unsigned char msg[] = {0x40, 0x04}; // Example CEC message
    mock.injectReceivedMessage(msg, sizeof(msg));

    // Optionally, simulate a transmit result being reported by the driver
    mock.simulateTxResult(/* txId */ 1, /* success */ true);

    // Verify that the mock's callbacks/handles have been set up as expected
    EXPECT_NE(mock.currentHandle, nullptr);
    EXPECT_TRUE(mock.rxCallback);

    // Additional EXPECT_CALL/ASSERT_* on your system-under-test can go here.
}
```

## Building

These mocks are built as part of the L1 and L2 test build process. The Makefile.am in tests/L1Tests includes these source files, compiling the fake AIDL service implementation into the run_L1Tests runner alongside the legacy driver mock, which stays last in that source list. The Makefile.am in tests/L2Tests builds two programs: the run_L2Tests runner, which carries the legacy driver mock, and a separate fake_hdmi_cec_aidl_host binary, which carries both fake sources. The fake belongs to the host, never to the runner, because the two have to be separate processes for real binder IPC to occur.

Neither production library source list references mocks/: not libRCEC_la_SOURCES in ccec/src/Makefile.am, and not OBJS in ccec/src/Makefile. That is why nothing in this directory can reach the shipped libRCEC library. The test Makefiles named above do reference it, and they are the only things that do, which is why a mocks/ reference must never be added to either production source list.
