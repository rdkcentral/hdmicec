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

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>
#include "ccec/CECFrame.hpp"
#include "ccec/FrameListener.hpp"
#include "ccec/Exception.hpp"
#include "hdmi_cec_driver_mock.h"

// Bus is internal, so we test through public APIs
#include "ccec/Connection.hpp"
#include "ccec/LibCCEC.hpp"

using ::testing::_;
using ::testing::Return;
using ::testing::Invoke;
using ::testing::DoAll;
using ::testing::SetArgPointee;

// Test fixture for Bus functionality tests
class BusTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Bus is already started by global environment
    }

    void TearDown() override {
        // Cleanup handled by global environment
    }
};

// Custom frame listener for testing
class TestFrameListener : public FrameListener {
public:
    TestFrameListener() : frameReceived(false) {}
    
    void notify(const CECFrame &frame) const override {
        frameReceived = true;
        lastFrame = frame;
    }
    
    mutable bool frameReceived;
    mutable CECFrame lastFrame;
};

// Thread-safe frame listener for tests where the Bus Reader thread dispatches the
// notification asynchronously.  Unlike TestFrameListener, notify() signals a
// condition variable so the test body can block until delivery is confirmed.
class SyncedFrameListener : public FrameListener {
public:
    SyncedFrameListener() : frameReceived(false) {}

    void notify(const CECFrame &frame) const override {
        {
            std::lock_guard<std::mutex> lk(mutex);
            frameReceived = true;
            lastFrame = frame;
        }
        cv.notify_one();
    }

    bool waitForFrame(int timeoutMs = 500) const {
        std::unique_lock<std::mutex> lk(mutex);
        return cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                           [this]{ return frameReceived; });
    }

    mutable bool frameReceived;
    mutable CECFrame lastFrame;
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
};

// Test basic send functionality with zero timeout
TEST_F(BusTest, SendFrameWithZeroTimeout) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    std::vector<unsigned char> capturedBuf;
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Invoke([&](int, const unsigned char* buf, int len, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            capturedBuf.assign(buf, buf + len);
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x40); // Playback 1 to TV header
    frame.append(0x36); // Standby opcode

    EXPECT_NO_THROW(conn.send(frame, 0));

    ASSERT_EQ(static_cast<int>(capturedBuf.size()), 2);
    EXPECT_EQ(capturedBuf[0], 0x40u); // header: src=PLAYBACK_DEVICE_1(4), dst=TV(0)
    EXPECT_EQ(capturedBuf[1], 0x36u); // Standby opcode

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test send functionality with timeout
TEST_F(BusTest, SendFrameWithTimeout) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    // timeout=500ms -> retry=2, but mock succeeds on the first attempt -> exactly 1 driver call.
    std::vector<unsigned char> capturedBuf;
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Invoke([&](int, const unsigned char* buf, int len, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            capturedBuf.assign(buf, buf + len);
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x40); // Playback 1 to TV header
    frame.append(0x84); // Report Physical Address opcode

    EXPECT_NO_THROW(conn.send(frame, 500));

    ASSERT_EQ(static_cast<int>(capturedBuf.size()), 2);
    EXPECT_EQ(capturedBuf[0], 0x40u);
    EXPECT_EQ(capturedBuf[1], 0x84u); // Report Physical Address opcode

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test sendAsync functionality
TEST_F(BusTest, SendFrameAsync) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    std::mutex callMutex;
    std::condition_variable callCv;
    bool txCalled = false;
    std::vector<unsigned char> capturedBuf;

    // sendAsync queues the frame; the Bus Writer thread delivers it via Driver::write -> HdmiCecTx.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Invoke([&](int, const unsigned char* buf, int len, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            {
                std::lock_guard<std::mutex> lk(callMutex);
                capturedBuf.assign(buf, buf + len);
                txCalled = true;
            }
            callCv.notify_one();
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);

    EXPECT_NO_THROW(conn.sendAsync(frame));

    {
        std::unique_lock<std::mutex> lk(callMutex);
        ASSERT_TRUE(callCv.wait_for(lk, std::chrono::milliseconds(500),
                                    [&]{ return txCalled; }))
            << "HdmiCecTx was not called within 500ms for sendAsync";
    }

    ASSERT_EQ(static_cast<int>(capturedBuf.size()), 2);
    EXPECT_EQ(capturedBuf[0], 0x40u);
    EXPECT_EQ(capturedBuf[1], 0x36u);

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test multiple async sends
TEST_F(BusTest, SendMultipleFramesAsync) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    std::mutex callMutex;
    std::condition_variable callCv;
    int callCount = 0;
    const int expectedCalls = 5;

    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(expectedCalls)
        .WillRepeatedly(Invoke([&](int, const unsigned char*, int, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            {
                std::lock_guard<std::mutex> lk(callMutex);
                ++callCount;
            }
            callCv.notify_one();
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    for (int i = 0; i < expectedCalls; i++) {
        CECFrame frame;
        frame.append(0x40);
        frame.append(0x36);
        EXPECT_NO_THROW(conn.sendAsync(frame));
    }

    {
        std::unique_lock<std::mutex> lk(callMutex);
        ASSERT_TRUE(callCv.wait_for(lk, std::chrono::milliseconds(500),
                                    [&]{ return callCount >= expectedCalls; }))
            << "Not all 5 async frames were delivered within 500ms";
    }

    EXPECT_EQ(callCount, expectedCalls);

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test poll functionality
TEST_F(BusTest, PollLogicalAddress) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    std::vector<unsigned char> capturedBuf;
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Invoke([&](int, const unsigned char* buf, int len, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            capturedBuf.assign(buf, buf + len);
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    // DriverImpl::poll builds a 1-byte frame: header = (from<<4)|to = (4<<4)|4 = 0x44
    EXPECT_NO_THROW(conn.poll(LogicalAddress::PLAYBACK_DEVICE_1, Throw_e()));

    ASSERT_EQ(static_cast<int>(capturedBuf.size()), 1);
    EXPECT_EQ(capturedBuf[0], 0x44u); // src=PLAYBACK_DEVICE_1(4), dst=PLAYBACK_DEVICE_1(4)

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test ping functionality
TEST_F(BusTest, PingLogicalAddress) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    std::vector<unsigned char> capturedBuf;
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Invoke([&](int, const unsigned char* buf, int len, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            capturedBuf.assign(buf, buf + len);
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    // DriverImpl::poll builds a 1-byte frame: header = (from<<4)|to = (4<<4)|0 = 0x40
    EXPECT_NO_THROW(conn.ping(LogicalAddress::PLAYBACK_DEVICE_1, LogicalAddress::TV, Throw_e()));

    ASSERT_EQ(static_cast<int>(capturedBuf.size()), 1);
    EXPECT_EQ(capturedBuf[0], 0x40u); // src=PLAYBACK_DEVICE_1(4), dst=TV(0)

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test frame listener functionality
TEST_F(BusTest, AddAndRemoveFrameListener) {
    Connection conn(LogicalAddress::UNREGISTERED, false);
    conn.open();
    
    TestFrameListener listener;
    
    EXPECT_NO_THROW({
        conn.addFrameListener(&listener);
    });
    
    EXPECT_NO_THROW({
        conn.removeFrameListener(&listener);
    });
    
    conn.close();
}

// Test multiple listeners
TEST_F(BusTest, MultipleFrameListeners) {
    Connection conn(LogicalAddress::UNREGISTERED, false);
    conn.open();
    
    TestFrameListener listener1;
    TestFrameListener listener2;
    TestFrameListener listener3;
    
    EXPECT_NO_THROW({
        conn.addFrameListener(&listener1);
        conn.addFrameListener(&listener2);
        conn.addFrameListener(&listener3);
    });
    
    EXPECT_NO_THROW({
        conn.removeFrameListener(&listener2);
        conn.removeFrameListener(&listener1);
        conn.removeFrameListener(&listener3);
    });
    
    conn.close();
}

// Test sending with specific logical addresses
TEST_F(BusTest, SendToSpecificAddress) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    std::vector<unsigned char> capturedBuf;
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Invoke([&](int, const unsigned char* buf, int len, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            capturedBuf.assign(buf, buf + len);
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x83); // Give Device Power Status opcode

    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame));

    // sendTo prepends the header: src=PLAYBACK_DEVICE_1(4), dst=TV(0) -> (4<<4)|0 = 0x40
    ASSERT_EQ(static_cast<int>(capturedBuf.size()), 2);
    EXPECT_EQ(capturedBuf[0], 0x40u); // header: src=4, dst=TV(0)
    EXPECT_EQ(capturedBuf[1], 0x83u); // Give Device Power Status opcode

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test sendToAsync with specific addresses
//
// Connection::sendToAsync → Connection::sendAsync → Bus::sendAsync → wQueue.
// The Bus Writer thread dequeues and calls Driver::write → HdmiCecTx.
// HdmiCecTxAsync (the driver's own one-shot async API) is NOT used in this path.
TEST_F(BusTest, SendToAsyncSpecificAddress) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    std::mutex callMutex;
    std::condition_variable callCv;
    bool txCalled = false;
    std::vector<unsigned char> capturedBuf;

    // Expect exactly one HdmiCecTx call from the Writer thread; capture the buffer
    // so we can verify frame contents after synchronisation.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .WillOnce(Invoke([&](int, const unsigned char* buf, int len, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            {
                std::lock_guard<std::mutex> lk(callMutex);
                capturedBuf.assign(buf, buf + len);
                txCalled = true;
            }
            callCv.notify_one();
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x83); // Give Device Power Status opcode

    EXPECT_NO_THROW({
        conn.sendToAsync(LogicalAddress::TV, frame);
    });

    // Block until the Writer thread dispatches to the driver, with a 500ms deadline.
    {
        std::unique_lock<std::mutex> lk(callMutex);
        ASSERT_TRUE(callCv.wait_for(lk, std::chrono::milliseconds(500),
                                    [&]{ return txCalled; }))
            << "HdmiCecTx was not called within 500ms for sendToAsync";
    }

    // Verify frame contents:
    //   byte 0 - header: src=PLAYBACK_DEVICE_1(4), dst=TV(0) => (4<<4)|0 = 0x40
    //   byte 1 - opcode: Give Device Power Status = 0x83
    ASSERT_EQ(static_cast<int>(capturedBuf.size()), 2);
    EXPECT_EQ(capturedBuf[0], 0x40u); // Header: src=4, dst=0
    EXPECT_EQ(capturedBuf[1], 0x83u); // Opcode: Give Device Power Status

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test send with throw exception parameter
TEST_F(BusTest, SendWithThrowParameter) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    // timeout=100ms -> retry=0 -> exactly 1 driver call; mock succeeds -> no exception.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(DoAll(
            SetArgPointee<3>(HDMI_CEC_IO_SENT_AND_ACKD),
            Return(HDMI_CEC_IO_SUCCESS)
        ));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);

    EXPECT_NO_THROW(conn.send(frame, 100, Throw_e()));

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test sendTo with throw exception parameter
TEST_F(BusTest, SendToWithThrowParameter) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    std::vector<unsigned char> capturedBuf;
    // timeout=100ms -> retry=0 -> exactly 1 driver call.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Invoke([&](int, const unsigned char* buf, int len, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            capturedBuf.assign(buf, buf + len);
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x83);

    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame, 100, Throw_e()));

    // Header prepended by sendTo: src=PLAYBACK_DEVICE_1(4), dst=TV(0) -> 0x40
    ASSERT_EQ(static_cast<int>(capturedBuf.size()), 2);
    EXPECT_EQ(capturedBuf[0], 0x40u);
    EXPECT_EQ(capturedBuf[1], 0x83u);

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test broadcast messaging
TEST_F(BusTest, SendBroadcastMessage) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    std::vector<unsigned char> capturedBuf;
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Invoke([&](int, const unsigned char* buf, int len, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            capturedBuf.assign(buf, buf + len);
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x82); // Active Source opcode
    frame.append(0x10);
    frame.append(0x00);

    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::BROADCAST, frame));

    // Header: src=PLAYBACK_DEVICE_1(4), dst=BROADCAST(0xF) -> (4<<4)|0xF = 0x4F
    ASSERT_EQ(static_cast<int>(capturedBuf.size()), 4);
    EXPECT_EQ(capturedBuf[0], 0x4Fu); // broadcast header
    EXPECT_EQ(capturedBuf[1], 0x82u); // Active Source opcode

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test connection filtering with specific logical address
TEST_F(BusTest, ConnectionWithSpecificLogicalAddress) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    
    EXPECT_NO_THROW({
        conn.open();
    });
    
    EXPECT_NO_THROW({
        conn.close();
    });
}

// Test multiple connections simultaneously
TEST_F(BusTest, MultipleConnections) {
    Connection conn1(LogicalAddress::PLAYBACK_DEVICE_1, false);
    Connection conn2(LogicalAddress::RECORDING_DEVICE_1, false);
    
    EXPECT_NO_THROW({
        conn1.open();
        conn2.open();
    });
    
    EXPECT_NO_THROW({
        conn1.close();
        conn2.close();
    });
}

// Test send with empty frame
TEST_F(BusTest, SendEmptyFrame) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    std::vector<unsigned char> capturedBuf;
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Invoke([&](int, const unsigned char* buf, int len, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            capturedBuf.assign(buf, buf + len);
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x40); // Header-only frame: src=PLAYBACK_DEVICE_1(4), dst=TV(0)

    EXPECT_NO_THROW(conn.send(frame, 0));

    // A header-only frame has exactly 1 byte with no opcode.
    ASSERT_EQ(static_cast<int>(capturedBuf.size()), 1);
    EXPECT_EQ(capturedBuf[0], 0x40u);

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test rapid open/close cycles
TEST_F(BusTest, RapidOpenCloseCycles) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    
    for (int i = 0; i < 3; i++) {
        EXPECT_NO_THROW({
            conn.open();
            conn.close();
        });
    }
}

// Verify that send with a positive timeout retries in 250ms increments and
// throws after all retries are exhausted.
//
// Bus::send retry math: retry = timeout / 250
//   The do-while loop runs (retry + 1) iterations maximum, so HdmiCecTx is
//   called exactly (retry + 1) times before the exception surfaces.
//   With timeout = 250 → retry = 1 → 2 driver calls, ~251ms elapsed.
TEST_F(BusTest, SendTimeoutExpiresAfterRetries) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    // timeout=250 → retry=1 → 2 calls total before the exception is re-thrown
    const int timeout = 250;
    const int expectedCalls = (timeout / 250) + 1; // 2

    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(expectedCalls)
        .WillRepeatedly(DoAll(
            SetArgPointee<3>(HDMI_CEC_IO_SENT_FAILED),
            Return(HDMI_CEC_IO_SUCCESS)
        ));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);

    auto start = std::chrono::steady_clock::now();

    // send with Throw_e surfaces the IOException once all retries are exhausted
    EXPECT_THROW(conn.send(frame, timeout, Throw_e()), Exception);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start).count();

    // The retry loop sleeps 250ms between each attempt, so elapsed must be
    // at least ~250ms (1 inter-retry sleep).  Allow 200ms lower bound for
    // scheduler variance.
    EXPECT_GE(elapsed, 200);

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Verify that send with a positive timeout succeeds as soon as the first
// successful retry returns, without waiting for the full timeout to expire.
//
// With timeout = 500 → retry = 2 → up to 3 driver calls.
// Configure: fail on attempt 1, succeed on attempt 2.
// Expected outcome: 2 driver calls, elapsed ~251ms — well under the 500ms window.
TEST_F(BusTest, SendSucceedsBeforeTimeoutExpiry) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(2)
        .WillOnce(DoAll(
            SetArgPointee<3>(HDMI_CEC_IO_SENT_FAILED),
            Return(HDMI_CEC_IO_SUCCESS)
        ))
        .WillOnce(DoAll(
            SetArgPointee<3>(HDMI_CEC_IO_SENT_AND_ACKD),
            Return(HDMI_CEC_IO_SUCCESS)
        ));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);

    auto start = std::chrono::steady_clock::now();

    // Should complete without throwing: success arrived before retry budget ran out
    EXPECT_NO_THROW(conn.send(frame, 500, Throw_e()));

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start).count();

    // Completed after 1 inter-retry sleep (~251ms), so must be strictly less
    // than the full 500ms timeout budget.
    EXPECT_LT(elapsed, 500);

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test listener receives frames addressed to its connection's logical address
TEST_F(BusTest, ListenerFiltering) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    SyncedFrameListener listener;

    conn.open();
    conn.addFrameListener(&listener);

    // Inject a frame addressed to PLAYBACK_DEVICE_1:
    //   from TV(0) to PLAYBACK_DEVICE_1(4) -> header = (0<<4)|4 = 0x04, opcode = Standby
    // DefaultFilter::isFiltered passes frames where header.to == connection source (4).
    unsigned char injectBuf[] = {0x04, 0x36};
    mock->injectReceivedMessage(injectBuf, 2);

    // Block until the Bus Reader thread dispatches the frame to our listener.
    ASSERT_TRUE(listener.waitForFrame(500))
        << "Listener was not notified within 500ms";
    EXPECT_TRUE(listener.frameReceived);

    conn.removeFrameListener(&listener);
    conn.close();
}

// Test unregistered connection receives all messages
TEST_F(BusTest, UnregisteredConnectionReceivesAll) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    Connection conn(LogicalAddress::UNREGISTERED, false);
    SyncedFrameListener listener;

    conn.open();
    conn.addFrameListener(&listener);

    // UNREGISTERED connections bypass DefaultFilter - any frame passes through.
    // Inject: from TV(0) to PLAYBACK_DEVICE_1(4), Standby opcode.
    unsigned char injectBuf[] = {0x04, 0x36};
    mock->injectReceivedMessage(injectBuf, 2);

    ASSERT_TRUE(listener.waitForFrame(500))
        << "UNREGISTERED listener was not notified within 500ms";
    EXPECT_TRUE(listener.frameReceived);

    conn.removeFrameListener(&listener);
    conn.close();
}

// Test send to same source address (loopback scenario)
TEST_F(BusTest, SendToSameAddress) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    std::vector<unsigned char> capturedBuf;
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Invoke([&](int, const unsigned char* buf, int len, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            capturedBuf.assign(buf, buf + len);
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x83);

    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::PLAYBACK_DEVICE_1, frame));

    // Header: src=PLAYBACK_DEVICE_1(4), dst=PLAYBACK_DEVICE_1(4) -> (4<<4)|4 = 0x44
    ASSERT_EQ(static_cast<int>(capturedBuf.size()), 2);
    EXPECT_EQ(capturedBuf[0], 0x44u); // loopback header: src==dst
    EXPECT_EQ(capturedBuf[1], 0x83u);

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test sequential sends
TEST_F(BusTest, SequentialSends) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    std::vector<unsigned char> callLog; // opcode byte per HdmiCecTx call, in arrival order
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(3)
        .WillRepeatedly(Invoke([&](int, const unsigned char* buf, int len, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            if (len >= 2) callLog.push_back(buf[1]);
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame1; frame1.append(0x40); frame1.append(0x83); // Give Device Power Status
    CECFrame frame2; frame2.append(0x40); frame2.append(0x36); // Standby
    CECFrame frame3; frame3.append(0x40); frame3.append(0x8F); // Give Device Power Status (query)

    EXPECT_NO_THROW({
        conn.send(frame1, 0);
        conn.send(frame2, 0);
        conn.send(frame3, 0);
    });

    // All 3 calls are synchronous on the calling thread - delivery order is guaranteed.
    ASSERT_EQ(static_cast<int>(callLog.size()), 3);
    EXPECT_EQ(callLog[0], 0x83u); // frame1 arrived first
    EXPECT_EQ(callLog[1], 0x36u); // frame2 arrived second
    EXPECT_EQ(callLog[2], 0x8Fu); // frame3 arrived third

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test mixed sync and async sends
//
// The interleaved sequence is: sync → async → sync → async.
// Ordering guarantees exercised:
//   (a) sync send #1 must be the very first driver call because no async work
//       was queued before it was issued.
//   (b) sync send #1 must be delivered before sync send #2 — both run on the
//       calling thread sequentially.
//   (c) async send #1 must be delivered before async send #2 — the Bus writer
//       thread drains wQueue in insertion (FIFO) order.
//
// Each send uses a distinct opcode so every entry in the captured log is
// uniquely identifiable:
//   frame_s1 (sync  #1): opcode 0x83
//   frame_a1 (async #1): opcode 0x36
//   frame_s2 (sync  #2): opcode 0x8F
//   frame_a2 (async #2): opcode 0x85
TEST_F(BusTest, MixedSyncAsyncSends) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    std::mutex logMutex;
    std::condition_variable logCv;
    std::vector<unsigned char> callLog; // opcode byte per HdmiCecTx call, in arrival order
    const int expectedCalls = 4;

    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(expectedCalls)
        .WillRepeatedly(Invoke([&](int, const unsigned char* buf, int len, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            {
                std::lock_guard<std::mutex> lk(logMutex);
                // buf[0] = header (src/dst), buf[1] = opcode
                if (len >= 2) callLog.push_back(buf[1]);
            }
            logCv.notify_one();
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame_s1; frame_s1.append(0x40); frame_s1.append(0x83); // sync  #1
    CECFrame frame_a1; frame_a1.append(0x40); frame_a1.append(0x36); // async #1
    CECFrame frame_s2; frame_s2.append(0x40); frame_s2.append(0x8F); // sync  #2
    CECFrame frame_a2; frame_a2.append(0x40); frame_a2.append(0x85); // async #2

    EXPECT_NO_THROW(conn.send(frame_s1, 0));
    EXPECT_NO_THROW(conn.sendAsync(frame_a1));
    EXPECT_NO_THROW(conn.send(frame_s2, 0));
    EXPECT_NO_THROW(conn.sendAsync(frame_a2));

    // Block until all 4 driver calls have been recorded, with a 500ms deadline.
    {
        std::unique_lock<std::mutex> lk(logMutex);
        ASSERT_TRUE(logCv.wait_for(lk, std::chrono::milliseconds(500),
                                   [&]{ return static_cast<int>(callLog.size()) >= expectedCalls; }))
            << "Not all 4 frames were delivered to the driver within 500ms";
    }

    ASSERT_EQ(static_cast<int>(callLog.size()), expectedCalls);

    // (a) First driver call must be sync send #1 — nothing was queued before it.
    EXPECT_EQ(callLog[0], 0x83u) << "sync send #1 must be the first driver call";

    // (b) Sync sends must arrive in source order (sequential on calling thread).
    auto pos_s1 = std::find(callLog.begin(), callLog.end(), static_cast<unsigned char>(0x83));
    auto pos_s2 = std::find(callLog.begin(), callLog.end(), static_cast<unsigned char>(0x8F));
    ASSERT_NE(pos_s1, callLog.end()) << "sync send #1 opcode not found in call log";
    ASSERT_NE(pos_s2, callLog.end()) << "sync send #2 opcode not found in call log";
    EXPECT_LT(pos_s1, pos_s2) << "sync send #1 must be delivered before sync send #2";

    // (c) Async sends must arrive in FIFO queue order relative to each other.
    auto pos_a1 = std::find(callLog.begin(), callLog.end(), static_cast<unsigned char>(0x36));
    auto pos_a2 = std::find(callLog.begin(), callLog.end(), static_cast<unsigned char>(0x85));
    ASSERT_NE(pos_a1, callLog.end()) << "async send #1 opcode not found in call log";
    ASSERT_NE(pos_a2, callLog.end()) << "async send #2 opcode not found in call log";
    EXPECT_LT(pos_a1, pos_a2) << "async send #1 must be delivered before async send #2 (FIFO queue)";

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test send with driver exception simulation
TEST_F(BusTest, SendWithDriverFailure) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    
    // Set up mock to fail on write
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SENT_FAILED));
    
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);
    
    // Should not throw even though driver fails
    EXPECT_NO_THROW({
        conn.send(frame, 0);
    });
    
    conn.close();
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test send with throw parameter when driver fails
TEST_F(BusTest, SendWithThrowOnDriverFailure) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    
    // Set up mock to fail on write
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SENT_FAILED));
    
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);
    
    // Should throw with Throw_e parameter
    EXPECT_THROW({
        conn.send(frame, 0, Throw_e());
    }, Exception);
    
    conn.close();
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test sendTo with throw parameter and driver failure
TEST_F(BusTest, SendToWithThrowOnDriverFailure) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    
    // Set up mock to fail on write
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SENT_FAILED));
    
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x83);
    
    // Should throw with Throw_e parameter
    EXPECT_THROW({
        conn.sendTo(LogicalAddress::TV, frame, 0, Throw_e());
    }, Exception);
    
    conn.close();
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test retry logic with timeout - eventual success
TEST_F(BusTest, SendWithTimeoutRetrySuccess) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    // timeout=500ms -> retry=2. Fail on attempt 1, succeed on attempt 2.
    // Bus::send sets retry=0 on the first success and exits -> exactly 2 driver calls.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(2)
        .WillOnce(Return(HDMI_CEC_IO_SENT_FAILED))
        .WillOnce(DoAll(
            SetArgPointee<3>(HDMI_CEC_IO_SENT_AND_ACKD),
            Return(HDMI_CEC_IO_SUCCESS)
        ));
    
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);
    
    EXPECT_NO_THROW(conn.send(frame, 500));
    
    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test poll with driver failure
TEST_F(BusTest, PollWithDriverFailure) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    
    // Set up mock to fail on poll
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SENT_BUT_NOT_ACKD));
    
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    // Poll should throw exception when it gets NACK
    EXPECT_THROW({
        conn.poll(LogicalAddress::PLAYBACK_DEVICE_1, Throw_e());
    }, Exception);
    
    conn.close();
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test ping with driver failure
TEST_F(BusTest, PingWithDriverFailure) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    
    // Set up mock to fail on ping
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SENT_BUT_NOT_ACKD));
    
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    // Ping should throw exception when it gets NACK
    EXPECT_THROW({
        conn.ping(LogicalAddress::PLAYBACK_DEVICE_1, LogicalAddress::TV, Throw_e());
    }, Exception);
    
    conn.close();
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test send with frame length > 1 for exception path
TEST_F(BusTest, SendLongerFrameWithFailure) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    
    // Set up mock to fail
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_GENERAL_ERROR));
    
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);
    frame.append(0x00); // Frame length > 1
    
    // Should not throw even with error (normal send swallows exceptions)
    EXPECT_NO_THROW({
        conn.send(frame, 0);
    });
    
    conn.close();
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test multiple consecutive async sends to exercise writer queue
TEST_F(BusTest, ManyAsyncSends) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    std::mutex callMutex;
    std::condition_variable callCv;
    int callCount = 0;
    const int expectedCalls = 20;

    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(expectedCalls)
        .WillRepeatedly(Invoke([&](int, const unsigned char*, int, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            {
                std::lock_guard<std::mutex> lk(callMutex);
                ++callCount;
            }
            callCv.notify_one();
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    for (int i = 0; i < expectedCalls; i++) {
        CECFrame frame;
        frame.append(0x40);
        frame.append(0x36);
        EXPECT_NO_THROW(conn.sendAsync(frame));
    }

    {
        std::unique_lock<std::mutex> lk(callMutex);
        ASSERT_TRUE(callCv.wait_for(lk, std::chrono::milliseconds(2000),
                                    [&]{ return callCount >= expectedCalls; }))
            << "Not all 20 async frames were delivered within 2000ms";
    }

    EXPECT_EQ(callCount, expectedCalls);

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test connection operations without bus started (negative test)
TEST_F(BusTest, OperationsWithoutBusStarted) {
    // Stop the bus first
    LibCCEC::getInstance().term();
    
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    
    // Operations should throw InvalidStateException
    EXPECT_THROW({
        conn.open();
    }, InvalidStateException);
    
    // Restart the bus for other tests
    LibCCEC::getInstance().init("CEC_TEST");
}

// Test send with very large timeout value
TEST_F(BusTest, SendWithLargeTimeout) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    // timeout=1000ms -> retry=4 (up to 5 attempts), but mock returns success immediately.
    // Bus::send sets retry=0 on the first success and exits -> exactly 1 driver call.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(DoAll(
            SetArgPointee<3>(HDMI_CEC_IO_SENT_AND_ACKD),
            Return(HDMI_CEC_IO_SUCCESS)
        ));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);

    EXPECT_NO_THROW(conn.send(frame, 1000));

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test async send followed by immediate close to test cleanup
TEST_F(BusTest, AsyncSendThenQuickClose) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    // The Writer thread runs independently of Connection::close. After 10ms the frame
    // will almost certainly have been delivered, but AtMost(1) bounds the call count
    // without imposing hard timing constraints on the test.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(::testing::AtMost(1))
        .WillRepeatedly(DoAll(
            SetArgPointee<3>(HDMI_CEC_IO_SENT_AND_ACKD),
            Return(HDMI_CEC_IO_SUCCESS)
        ));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);

    EXPECT_NO_THROW(conn.sendAsync(frame));

    usleep(10000); // Allow the Writer thread time to dequeue and deliver the frame
    EXPECT_NO_THROW(conn.close());

    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test broadcast send with timeout
TEST_F(BusTest, BroadcastSendWithTimeout) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    std::vector<unsigned char> capturedBuf;
    // timeout=250ms -> retry=1, mock succeeds first attempt -> exactly 1 driver call.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Invoke([&](int, const unsigned char* buf, int len, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            capturedBuf.assign(buf, buf + len);
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x82); // Active Source
    frame.append(0x10);
    frame.append(0x00);

    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::BROADCAST, frame, 250));

    // Header: src=PLAYBACK_DEVICE_1(4), dst=BROADCAST(0xF) -> (4<<4)|0xF = 0x4F
    ASSERT_EQ(static_cast<int>(capturedBuf.size()), 4);
    EXPECT_EQ(capturedBuf[0], 0x4Fu); // broadcast header
    EXPECT_EQ(capturedBuf[1], 0x82u); // Active Source opcode

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test multiple timeouts with same frame
TEST_F(BusTest, MultipleTimeoutRetries) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    ASSERT_NE(mock, nullptr);

    // Each send uses timeout=250ms -> retry=1; mock succeeds on first attempt each time
    // -> exactly 1 driver call per send, 3 total.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(3)
        .WillRepeatedly(DoAll(
            SetArgPointee<3>(HDMI_CEC_IO_SENT_AND_ACKD),
            Return(HDMI_CEC_IO_SUCCESS)
        ));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x40);
    frame.append(0x83);

    EXPECT_NO_THROW(conn.send(frame, 250));
    EXPECT_NO_THROW(conn.send(frame, 250));
    EXPECT_NO_THROW(conn.send(frame, 250));

    conn.close();
    ::testing::Mock::VerifyAndClearExpectations(mock);
}
