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
#include <condition_variable>
#include <mutex>
#include <vector>
#include "ccec/Connection.hpp"
#include "ccec/Exception.hpp"
#include "hdmi_cec_driver_mock.h"

using ::testing::_;
using ::testing::Return;
using ::testing::DoAll;
using ::testing::Invoke;
using ::testing::SetArgPointee;

class ConnectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Bus is already started by global test environment
    }

    void TearDown() override {
        // Cleanup handled by global test environment
    }
};

// Helper frame listener for testing
class TestFrameListener : public FrameListener {
public:
    TestFrameListener() : frameCount(0) {}
    
    void notify(const CECFrame &frame) const override {
        frameCount++;
        lastFrame = frame;
    }
    
    mutable int frameCount;
    mutable CECFrame lastFrame;
};

// Test basic constructor with auto-open
TEST_F(ConnectionTest, ConstructorWithAutoOpen) {
    EXPECT_NO_THROW({
        Connection conn(LogicalAddress::UNREGISTERED, true);
        conn.close();
    });
}

// Test constructor without auto-open
TEST_F(ConnectionTest, ConstructorWithoutAutoOpen) {
    Connection conn(LogicalAddress::UNREGISTERED, false);
    // Even though not opened, explicitly verify no issues
    EXPECT_EQ(conn.getSource().toInt(), LogicalAddress::UNREGISTERED);
}

// Test constructor with named connection
TEST_F(ConnectionTest, ConstructorWithName) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false, "TestConnection");
    EXPECT_EQ(conn.getSource().toInt(), LogicalAddress::PLAYBACK_DEVICE_1);
}

// Test constructor with specific logical address
TEST_F(ConnectionTest, ConstructorWithLogicalAddress) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    EXPECT_EQ(conn.getSource().toInt(), LogicalAddress::PLAYBACK_DEVICE_1);
}

// Test open and close
TEST_F(ConnectionTest, OpenAndClose) {
    Connection conn(LogicalAddress::UNREGISTERED, false);
    EXPECT_NO_THROW(conn.open());
    EXPECT_NO_THROW(conn.close());
}

// Test multiple open/close cycles
TEST_F(ConnectionTest, MultipleOpenClose) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    
    for (int i = 0; i < 3; i++) {
        EXPECT_NO_THROW(conn.open());
        EXPECT_NO_THROW(conn.close());
    }
}

// Test add and remove frame listener
TEST_F(ConnectionTest, AddAndRemoveFrameListener) {
    Connection conn(LogicalAddress::UNREGISTERED, false);
    conn.open();
    
    TestFrameListener listener;
    EXPECT_NO_THROW(conn.addFrameListener(&listener));
    EXPECT_NO_THROW(conn.removeFrameListener(&listener));
    
    conn.close();
}

// Test multiple frame listeners
TEST_F(ConnectionTest, MultipleFrameListeners) {
    Connection conn(LogicalAddress::UNREGISTERED, false);
    conn.open();
    
    TestFrameListener listener1, listener2, listener3;
    
    EXPECT_NO_THROW(conn.addFrameListener(&listener1));
    EXPECT_NO_THROW(conn.addFrameListener(&listener2));
    EXPECT_NO_THROW(conn.addFrameListener(&listener3));
    
    EXPECT_NO_THROW(conn.removeFrameListener(&listener2));
    EXPECT_NO_THROW(conn.removeFrameListener(&listener1));
    EXPECT_NO_THROW(conn.removeFrameListener(&listener3));
    
    conn.close();
}

// Test send with timeout
TEST_F(ConnectionTest, SendWithTimeout) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
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
    frame.append(0x40);
    frame.append(0x36); // Standby

    EXPECT_NO_THROW(conn.send(frame, 100));
    conn.close();

    ASSERT_EQ(capturedBuf.size(), (size_t)2);
    EXPECT_EQ(capturedBuf[0], 0x40); // src=4, dst=0
    EXPECT_EQ(capturedBuf[1], 0x36); // Standby opcode
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test send with zero timeout
TEST_F(ConnectionTest, SendWithZeroTimeout) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
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
    frame.append(0x40);
    frame.append(0x83);

    EXPECT_NO_THROW(conn.send(frame, 0));
    conn.close();

    ASSERT_EQ(capturedBuf.size(), (size_t)2);
    EXPECT_EQ(capturedBuf[0], 0x40);
    EXPECT_EQ(capturedBuf[1], 0x83);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test send with default timeout
TEST_F(ConnectionTest, SendWithDefaultTimeout) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
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
    frame.append(0x40);
    frame.append(0x36);

    EXPECT_NO_THROW(conn.send(frame));
    conn.close();

    ASSERT_EQ(capturedBuf.size(), (size_t)2);
    EXPECT_EQ(capturedBuf[0], 0x40);
    EXPECT_EQ(capturedBuf[1], 0x36);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test send with throw parameter succeeds when driver succeeds
TEST_F(ConnectionTest, SendWithThrowParameter) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
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
    frame.append(0x40);
    frame.append(0x36);

    // Throw_e() means exceptions surface; driver succeeds so no throw expected
    EXPECT_NO_THROW(conn.send(frame, 100, Throw_e()));
    conn.close();

    ASSERT_EQ(capturedBuf.size(), (size_t)2);
    EXPECT_EQ(capturedBuf[0], 0x40);
    EXPECT_EQ(capturedBuf[1], 0x36);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test sendTo with specific address
// sendTo prepends a Header: src=PLAYBACK_DEVICE_1(4), dst=TV(0) => 0x40
TEST_F(ConnectionTest, SendToSpecificAddress) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
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
    frame.append(0x83); // Give Device Power Status

    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame));
    conn.close();

    // sendTo prepends header: src=4, dst=0 => 0x40
    ASSERT_EQ(capturedBuf.size(), (size_t)2);
    EXPECT_EQ(capturedBuf[0], 0x40);
    EXPECT_EQ(capturedBuf[1], 0x83);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test sendTo with timeout
TEST_F(ConnectionTest, SendToWithTimeout) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
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

    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame, 200));
    conn.close();

    ASSERT_EQ(capturedBuf.size(), (size_t)2);
    EXPECT_EQ(capturedBuf[0], 0x40); // src=4, dst=0
    EXPECT_EQ(capturedBuf[1], 0x83);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test sendTo with throw parameter succeeds when driver succeeds
TEST_F(ConnectionTest, SendToWithThrowParameter) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
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

    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame, 100, Throw_e()));
    conn.close();

    ASSERT_EQ(capturedBuf.size(), (size_t)2);
    EXPECT_EQ(capturedBuf[0], 0x40);
    EXPECT_EQ(capturedBuf[1], 0x83);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test sendToAsync — verify header is constructed (src=4, dst=0 => 0x40) and opcode preserved
TEST_F(ConnectionTest, SendToAsync) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<unsigned char> capturedBuf;
    bool txCalled = false;

    EXPECT_CALL(*mock, HdmiCecTxAsync(_, _, _))
        .Times(1)
        .WillOnce(Invoke([&](int, const unsigned char* buf, int len) -> int {
            std::lock_guard<std::mutex> lk(mtx);
            capturedBuf.assign(buf, buf + len);
            txCalled = true;
            cv.notify_one();
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x36); // Standby

    EXPECT_NO_THROW(conn.sendToAsync(LogicalAddress::TV, frame));

    {
        std::unique_lock<std::mutex> lk(mtx);
        ASSERT_TRUE(cv.wait_for(lk, std::chrono::milliseconds(500), [&]{ return txCalled; }))
            << "HdmiCecTxAsync was not called within timeout";
    }
    conn.close();

    // sendToAsync prepends header: src=4, dst=0 => 0x40
    ASSERT_EQ(capturedBuf.size(), (size_t)2);
    EXPECT_EQ(capturedBuf[0], 0x40);
    EXPECT_EQ(capturedBuf[1], 0x36);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test sendAsync — frame already has header bytes; verify driver receives them unchanged
TEST_F(ConnectionTest, SendAsync) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<unsigned char> capturedBuf;
    bool txCalled = false;

    EXPECT_CALL(*mock, HdmiCecTxAsync(_, _, _))
        .Times(1)
        .WillOnce(Invoke([&](int, const unsigned char* buf, int len) -> int {
            std::lock_guard<std::mutex> lk(mtx);
            capturedBuf.assign(buf, buf + len);
            txCalled = true;
            cv.notify_one();
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);

    EXPECT_NO_THROW(conn.sendAsync(frame));

    {
        std::unique_lock<std::mutex> lk(mtx);
        ASSERT_TRUE(cv.wait_for(lk, std::chrono::milliseconds(500), [&]{ return txCalled; }))
            << "HdmiCecTxAsync was not called within timeout";
    }
    conn.close();

    ASSERT_EQ(capturedBuf.size(), (size_t)2);
    EXPECT_EQ(capturedBuf[0], 0x40);
    EXPECT_EQ(capturedBuf[1], 0x36);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test poll — poll(addr) sends a 1-byte header with src==dst: PLAYBACK_DEVICE_1(4)->4 => 0x44
TEST_F(ConnectionTest, PollAddress) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
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

    EXPECT_NO_THROW(conn.poll(LogicalAddress::PLAYBACK_DEVICE_1, Throw_e()));
    conn.close();

    // poll frame is header-only: src=4, dst=4 => 0x44
    ASSERT_EQ(capturedBuf.size(), (size_t)1);
    EXPECT_EQ(capturedBuf[0], 0x44);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test ping — ping(from, to) sends a 1-byte header: src=PLAYBACK_DEVICE_1(4), dst=TV(0) => 0x40
TEST_F(ConnectionTest, PingAddress) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
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

    EXPECT_NO_THROW(conn.ping(LogicalAddress::PLAYBACK_DEVICE_1, LogicalAddress::TV, Throw_e()));
    conn.close();

    // ping frame is header-only: src=4, dst=0 => 0x40
    ASSERT_EQ(capturedBuf.size(), (size_t)1);
    EXPECT_EQ(capturedBuf[0], 0x40);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test getSource
TEST_F(ConnectionTest, GetSource) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    LogicalAddress source = conn.getSource();
    EXPECT_EQ(source.toInt(), LogicalAddress::PLAYBACK_DEVICE_1);
    // Connection destructor will be called automatically
}

// Test setSource
TEST_F(ConnectionTest, SetSource) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.setSource(LogicalAddress::PLAYBACK_DEVICE_2);
    LogicalAddress source = conn.getSource();
    EXPECT_EQ(source.toInt(), LogicalAddress::PLAYBACK_DEVICE_2);
    // Connection destructor will be called automatically
}

// Test broadcast message — dst=BROADCAST(15=0xF): src=4, dst=F => header=0x4F
TEST_F(ConnectionTest, SendBroadcast) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
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
    frame.append(0x82); // Active Source
    frame.append(0x10);
    frame.append(0x00);

    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::BROADCAST, frame));
    conn.close();

    // sendTo prepends header: src=4, dst=15 => 0x4F
    ASSERT_GE(capturedBuf.size(), (size_t)1);
    EXPECT_EQ(capturedBuf[0], 0x4F);
    EXPECT_EQ(capturedBuf[1], 0x82);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test sending to same address — src=4, dst=4 => header=0x44
TEST_F(ConnectionTest, SendToSameAddress) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
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
    conn.close();

    // src=4, dst=4 => 0x44
    ASSERT_GE(capturedBuf.size(), (size_t)1);
    EXPECT_EQ(capturedBuf[0], 0x44);
    EXPECT_EQ(capturedBuf[1], 0x83);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test multiple sends — verify all 3 frames were delivered in order
TEST_F(ConnectionTest, MultipleSends) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    std::vector<unsigned char> opcodes;
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(3)
        .WillRepeatedly(Invoke([&](int, const unsigned char* buf, int len, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            if (len >= 2) opcodes.push_back(buf[1]);
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame1; frame1.append(0x83);
    CECFrame frame2; frame2.append(0x36);
    CECFrame frame3; frame3.append(0x8F);

    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame1));
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame2));
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame3));
    conn.close();

    // All three opcodes delivered in order
    ASSERT_EQ(opcodes.size(), (size_t)3);
    EXPECT_EQ(opcodes[0], 0x83);
    EXPECT_EQ(opcodes[1], 0x36);
    EXPECT_EQ(opcodes[2], 0x8F);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test send with different addresses — verify each header byte has the correct dst nibble
TEST_F(ConnectionTest, SendToDifferentAddresses) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    std::vector<unsigned char> headers;
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(3)
        .WillRepeatedly(Invoke([&](int, const unsigned char* buf, int len, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            if (len >= 1) headers.push_back(buf[0]);
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame; frame.append(0x83);

    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame));             // dst=0 => 0x40
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::AUDIO_SYSTEM, frame));   // dst=5 => 0x45
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::RECORDING_DEVICE_1, frame)); // dst=1 => 0x41
    conn.close();

    ASSERT_EQ(headers.size(), (size_t)3);
    EXPECT_EQ(headers[0], 0x40); // src=4, dst=TV(0)
    EXPECT_EQ(headers[1], 0x45); // src=4, dst=AUDIO_SYSTEM(5)
    EXPECT_EQ(headers[2], 0x41); // src=4, dst=RECORDING_DEVICE_1(1)
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test connection with different logical addresses
TEST_F(ConnectionTest, DifferentLogicalAddresses) {
    {
        Connection conn1(LogicalAddress::TV, false);
        EXPECT_EQ(conn1.getSource().toInt(), LogicalAddress::TV);
    }
    {
        Connection conn2(LogicalAddress::PLAYBACK_DEVICE_1, false);
        EXPECT_EQ(conn2.getSource().toInt(), LogicalAddress::PLAYBACK_DEVICE_1);
    }
    {
        Connection conn3(LogicalAddress::AUDIO_SYSTEM, false);
        EXPECT_EQ(conn3.getSource().toInt(), LogicalAddress::AUDIO_SYSTEM);
    }
}

// Test close clears frame listeners
TEST_F(ConnectionTest, CloseRemovesListeners) {
    Connection conn(LogicalAddress::UNREGISTERED, false);
    conn.open();
    
    TestFrameListener listener1, listener2;
    conn.addFrameListener(&listener1);
    conn.addFrameListener(&listener2);
    
    // Close should clear listeners
    EXPECT_NO_THROW(conn.close());
    
    // Should be able to open again and add new listeners
    EXPECT_NO_THROW(conn.open());
    EXPECT_NO_THROW(conn.addFrameListener(&listener1));
    
    conn.close();
}

// Test sendAsync multiple times — verify all 5 frames reach the driver
TEST_F(ConnectionTest, MultipleAsyncSends) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    std::mutex mtx;
    std::condition_variable cv;
    int callCount = 0;

    EXPECT_CALL(*mock, HdmiCecTxAsync(_, _, _))
        .Times(5)
        .WillRepeatedly(Invoke([&](int, const unsigned char*, int) -> int {
            std::lock_guard<std::mutex> lk(mtx);
            ++callCount;
            cv.notify_one();
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    for (int i = 0; i < 5; i++) {
        CECFrame frame;
        frame.append(0x36);
        EXPECT_NO_THROW(conn.sendAsync(frame));
    }

    {
        std::unique_lock<std::mutex> lk(mtx);
        ASSERT_TRUE(cv.wait_for(lk, std::chrono::milliseconds(1000), [&]{ return callCount == 5; }))
            << "Not all 5 async sends completed; got " << callCount;
    }
    conn.close();

    EXPECT_EQ(callCount, 5);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test sendToAsync to different addresses — verify each header's dst nibble is correct
TEST_F(ConnectionTest, AsyncSendToDifferentAddresses) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<unsigned char> headers;

    EXPECT_CALL(*mock, HdmiCecTxAsync(_, _, _))
        .Times(3)
        .WillRepeatedly(Invoke([&](int, const unsigned char* buf, int len) -> int {
            std::lock_guard<std::mutex> lk(mtx);
            if (len >= 1) headers.push_back(buf[0]);
            cv.notify_all();
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame; frame.append(0x83);

    EXPECT_NO_THROW(conn.sendToAsync(LogicalAddress::TV, frame));        // dst=0 => 0x40
    EXPECT_NO_THROW(conn.sendToAsync(LogicalAddress::AUDIO_SYSTEM, frame)); // dst=5 => 0x45
    EXPECT_NO_THROW(conn.sendToAsync(LogicalAddress::BROADCAST, frame));    // dst=15 => 0x4F

    {
        std::unique_lock<std::mutex> lk(mtx);
        ASSERT_TRUE(cv.wait_for(lk, std::chrono::milliseconds(1000), [&]{ return headers.size() == 3; }))
            << "Not all 3 async sends completed; got " << headers.size();
    }
    conn.close();

    EXPECT_EQ(headers[0], 0x40); // TV
    EXPECT_EQ(headers[1], 0x45); // AUDIO_SYSTEM
    EXPECT_EQ(headers[2], 0x4F); // BROADCAST
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test empty frame send - should throw exception
TEST_F(ConnectionTest, SendEmptyFrameThrows) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame emptyFrame;
    // Empty frame should throw because matchSource tries to access buf[0]
    EXPECT_THROW(conn.send(emptyFrame), std::out_of_range);
    
    conn.close();
}

// Test unregistered connection (receives all messages)
TEST_F(ConnectionTest, UnregisteredConnectionFiltering) {
    Connection conn(LogicalAddress::UNREGISTERED, false);
    conn.open();
    
    TestFrameListener listener;
    EXPECT_NO_THROW(conn.addFrameListener(&listener));
    EXPECT_NO_THROW(conn.removeFrameListener(&listener));
    
    conn.close();
}

// Test with various timeout values — verify driver is called exactly once per send regardless of timeout
TEST_F(ConnectionTest, VariousTimeouts) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    int callCount = 0;
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(5)
        .WillRepeatedly(Invoke([&](int, const unsigned char*, int, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            ++callCount;
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x36);

    // Each send with a different timeout should still result in exactly 1 driver call
    EXPECT_NO_THROW(conn.send(frame, 0));
    EXPECT_NO_THROW(conn.send(frame, 50));
    EXPECT_NO_THROW(conn.send(frame, 100));
    EXPECT_NO_THROW(conn.send(frame, 250));
    EXPECT_NO_THROW(conn.send(frame, 500));
    conn.close();

    EXPECT_EQ(callCount, 5);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test sendTo with various timeout values — verify driver called exactly once per send
TEST_F(ConnectionTest, SendToVariousTimeouts) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    int callCount = 0;
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(3)
        .WillRepeatedly(Invoke([&](int, const unsigned char*, int, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            ++callCount;
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame; frame.append(0x83);

    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame, 0));
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame, 100));
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame, 500));
    conn.close();

    EXPECT_EQ(callCount, 3);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test connection remains functional after multiple sends — all 10 must reach the driver
TEST_F(ConnectionTest, ConnectionAfterSend) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    int callCount = 0;
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(10)
        .WillRepeatedly(Invoke([&](int, const unsigned char*, int, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            ++callCount;
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame; frame.append(0x36);

    for (int i = 0; i < 10; i++) {
        EXPECT_NO_THROW(conn.send(frame, 0));
    }
    conn.close();

    EXPECT_EQ(callCount, 10);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test mixed sync and async operations — verify both sync (HdmiCecTx) and async (HdmiCecTxAsync) calls reach the driver
TEST_F(ConnectionTest, MixedSyncAsync) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    std::mutex mtx;
    std::condition_variable cv;
    int syncCount = 0;
    int asyncCount = 0;

    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(2)
        .WillRepeatedly(Invoke([&](int, const unsigned char*, int, int* result) -> int {
            if (result) *result = HDMI_CEC_IO_SENT_AND_ACKD;
            ++syncCount;
            return HDMI_CEC_IO_SUCCESS;
        }));

    EXPECT_CALL(*mock, HdmiCecTxAsync(_, _, _))
        .Times(2)
        .WillRepeatedly(Invoke([&](int, const unsigned char*, int) -> int {
            std::lock_guard<std::mutex> lk(mtx);
            ++asyncCount;
            cv.notify_all();
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame; frame.append(0x36);

    EXPECT_NO_THROW(conn.send(frame, 0));
    EXPECT_NO_THROW(conn.sendAsync(frame));
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame));
    EXPECT_NO_THROW(conn.sendToAsync(LogicalAddress::TV, frame));

    {
        std::unique_lock<std::mutex> lk(mtx);
        ASSERT_TRUE(cv.wait_for(lk, std::chrono::milliseconds(1000), [&]{ return asyncCount == 2; }))
            << "Async sends did not complete; asyncCount=" << asyncCount;
    }
    conn.close();

    EXPECT_EQ(syncCount, 2)  << "Expected 2 synchronous driver calls";
    EXPECT_EQ(asyncCount, 2) << "Expected 2 asynchronous driver calls";
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test listener operations while connection is closed
TEST_F(ConnectionTest, ListenerOperationsWithoutOpen) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    TestFrameListener listener;
    // These should work even if connection isn't opened
    EXPECT_NO_THROW(conn.addFrameListener(&listener));
    EXPECT_NO_THROW(conn.removeFrameListener(&listener));
    // Connection destructor will be called automatically
}

// Test remove non-existent listener
TEST_F(ConnectionTest, RemoveNonExistentListener) {
    Connection conn(LogicalAddress::UNREGISTERED, false);
    conn.open();
    
    TestFrameListener listener;
    // Remove without adding - should not crash
    EXPECT_NO_THROW(conn.removeFrameListener(&listener));
    
    conn.close();
}

// Test send with Throw_e and driver failure to trigger exception path
TEST_F(ConnectionTest, SendWithThrowAndDriverFailure) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    
    // Set up mock to fail
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SENT_FAILED));
    
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x36);
    
    // Should throw when driver fails
    EXPECT_THROW({
        conn.send(frame, 0, Throw_e());
    }, Exception);
    
    conn.close();
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test sendTo with Throw_e and driver failure to trigger exception path
TEST_F(ConnectionTest, SendToWithThrowAndDriverFailure) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    
    // Set up mock to fail
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SENT_FAILED));
    
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x83);
    
    // Should throw when driver fails
    EXPECT_THROW({
        conn.sendTo(LogicalAddress::TV, frame, 0, Throw_e());
    }, Exception);
    
    conn.close();
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test poll with Throw_e and driver failure to trigger exception path
TEST_F(ConnectionTest, PollWithThrowAndDriverFailure) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    
    // Set up mock to fail
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SENT_BUT_NOT_ACKD));
    
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    // Should throw when driver fails (NACK)
    EXPECT_THROW({
        conn.poll(LogicalAddress::PLAYBACK_DEVICE_1, Throw_e());
    }, Exception);
    
    conn.close();
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test ping with Throw_e and driver failure to trigger exception path
TEST_F(ConnectionTest, PingWithThrowAndDriverFailure) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    
    // Set up mock to fail
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SENT_BUT_NOT_ACKD));
    
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    // Should throw when driver fails (NACK)
    EXPECT_THROW({
        conn.ping(LogicalAddress::PLAYBACK_DEVICE_1, LogicalAddress::TV, Throw_e());
    }, Exception);
    
    conn.close();
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test send with exception swallowing (no Throw_e parameter)
TEST_F(ConnectionTest, SendWithoutThrowSwallowsException) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    
    // Set up mock to fail
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_GENERAL_ERROR));
    
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x36);
    
    // Should NOT throw - exception is swallowed
    EXPECT_NO_THROW({
        conn.send(frame, 0);
    });
    
    conn.close();
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test sendTo with exception swallowing (no Throw_e parameter)
TEST_F(ConnectionTest, SendToWithoutThrowSwallowsException) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    
    // Set up mock to fail
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_GENERAL_ERROR));
    
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x83);
    
    // Should NOT throw - exception is swallowed
    EXPECT_NO_THROW({
        conn.sendTo(LogicalAddress::TV, frame, 0);
    });
    
    conn.close();
    
    // Clear mock expectations
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test matchSource with valid logical address — source nibble already matches, must be preserved
TEST_F(ConnectionTest, MatchSourceValidAddress) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
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

    // Frame already has correct source nibble (4=PLAYBACK_DEVICE_1)
    CECFrame frame;
    frame.append(0x4F); // src=4, dst=F; source nibble is correct
    frame.append(0x36);

    EXPECT_NO_THROW(conn.send(frame, 0));
    conn.close();

    // Source nibble must remain 4 (PLAYBACK_DEVICE_1)
    ASSERT_GE(capturedBuf.size(), (size_t)1);
    EXPECT_EQ((capturedBuf[0] >> 4) & 0x0F, 0x4) << "Source nibble should be PLAYBACK_DEVICE_1 (4)";
    EXPECT_EQ(capturedBuf[0] & 0x0F, 0xF)        << "Destination nibble should be unchanged";
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test matchSource with mismatched source — the source nibble must be corrected to conn's address
TEST_F(ConnectionTest, MatchSourceMismatchedAddress) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
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

    // Frame has wrong source (0=TV); matchSource should rewrite it to 4=PLAYBACK_DEVICE_1
    CECFrame frame;
    frame.append(0x0F); // src=0 (TV), dst=F — source is wrong
    frame.append(0x36);

    EXPECT_NO_THROW(conn.send(frame, 0));
    conn.close();

    // After matchSource correction: src nibble should be 4, dst nibble unchanged (F)
    ASSERT_GE(capturedBuf.size(), (size_t)1);
    EXPECT_EQ((capturedBuf[0] >> 4) & 0x0F, 0x4) << "Source nibble should be corrected to PLAYBACK_DEVICE_1 (4)";
    EXPECT_EQ(capturedBuf[0] & 0x0F, 0xF)        << "Destination nibble should remain F";
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test DefaultFilter with UNREGISTERED source — unregistered connections receive all messages
TEST_F(ConnectionTest, DefaultFilterUnregistered) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Connection conn(LogicalAddress::UNREGISTERED, false);
    conn.open();

    TestFrameListener listener;
    conn.addFrameListener(&listener);

    // Inject any frame via the RX callback; UNREGISTERED connections receive all
    unsigned char buf[] = {0x04, 0x36}; // src=TV, dst=PLAYBACK_DEVICE_1
    mock->injectReceivedMessage(buf, sizeof(buf));
    usleep(50000);

    EXPECT_GT(listener.frameCount, 0) << "Unregistered connection should have received the frame";

    conn.removeFrameListener(&listener);
    conn.close();
}

// Test DefaultFilter with specific address — frame addressed to this device should be received
TEST_F(ConnectionTest, DefaultFilterSpecificAddress) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    TestFrameListener listener;
    conn.addFrameListener(&listener);

    // Inject frame addressed to PLAYBACK_DEVICE_1 (dst=4): src=TV(0), dst=4 => 0x04
    unsigned char buf[] = {0x04, 0x36};
    mock->injectReceivedMessage(buf, sizeof(buf));
    usleep(50000);

    EXPECT_GT(listener.frameCount, 0) << "Frame addressed to this device should have been delivered";

    conn.removeFrameListener(&listener);
    conn.close();
}

// Test DefaultFilter with broadcast message — broadcast frames should reach any device
TEST_F(ConnectionTest, DefaultFilterBroadcast) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    TestFrameListener listener;
    conn.addFrameListener(&listener);

    // Inject broadcast frame: src=TV(0), dst=BROADCAST(15=F) => 0x0F
    unsigned char buf[] = {0x0F, 0x82};
    mock->injectReceivedMessage(buf, sizeof(buf));
    usleep(50000);

    EXPECT_GT(listener.frameCount, 0) << "Broadcast frame should have been delivered to this connection";

    conn.removeFrameListener(&listener);
    conn.close();
}

// Test DefaultFilter with filtered message — frame for a different device should NOT reach this connection
TEST_F(ConnectionTest, DefaultFilterFiltered) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    TestFrameListener listener;
    conn.addFrameListener(&listener);

    // Inject frame addressed to AUDIO_SYSTEM (dst=5), not PLAYBACK_DEVICE_1 (4)
    unsigned char buf[] = {0x05, 0x36}; // src=TV(0), dst=AUDIO_SYSTEM(5)
    mock->injectReceivedMessage(buf, sizeof(buf));
    usleep(50000);

    EXPECT_EQ(listener.frameCount, 0) << "Frame for a different device should have been filtered out";

    conn.removeFrameListener(&listener);
    conn.close();
}

// Test multiple listeners notification — all registered listeners receive the same injected frame
TEST_F(ConnectionTest, MultipleListenersNotification) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    TestFrameListener listener1;
    TestFrameListener listener2;
    TestFrameListener listener3;

    conn.addFrameListener(&listener1);
    conn.addFrameListener(&listener2);
    conn.addFrameListener(&listener3);

    // Inject a frame addressed to PLAYBACK_DEVICE_1 (dst=4): src=TV(0) => 0x04
    unsigned char buf[] = {0x04, 0x36};
    mock->injectReceivedMessage(buf, sizeof(buf));
    usleep(50000);

    EXPECT_GT(listener1.frameCount, 0) << "listener1 should have been notified";
    EXPECT_GT(listener2.frameCount, 0) << "listener2 should have been notified";
    EXPECT_GT(listener3.frameCount, 0) << "listener3 should have been notified";

    conn.removeFrameListener(&listener1);
    conn.removeFrameListener(&listener2);
    conn.removeFrameListener(&listener3);
    conn.close();
}

// Test sendAsync with matchSource correction — wrong source nibble should be corrected to 4
TEST_F(ConnectionTest, SendAsyncMatchSource) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<unsigned char> capturedBuf;
    bool txCalled = false;

    EXPECT_CALL(*mock, HdmiCecTxAsync(_, _, _))
        .Times(1)
        .WillOnce(Invoke([&](int, const unsigned char* buf, int len) -> int {
            std::lock_guard<std::mutex> lk(mtx);
            capturedBuf.assign(buf, buf + len);
            txCalled = true;
            cv.notify_one();
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    // Frame has wrong source (0=TV); matchSource should rewrite to 4=PLAYBACK_DEVICE_1
    CECFrame frame;
    frame.append(0x0F); // src=0 (wrong), dst=F
    frame.append(0x36);

    EXPECT_NO_THROW(conn.sendAsync(frame));

    {
        std::unique_lock<std::mutex> lk(mtx);
        ASSERT_TRUE(cv.wait_for(lk, std::chrono::milliseconds(500), [&]{ return txCalled; }))
            << "HdmiCecTxAsync was not called within timeout";
    }
    conn.close();

    ASSERT_GE(capturedBuf.size(), (size_t)1);
    EXPECT_EQ((capturedBuf[0] >> 4) & 0x0F, 0x4) << "Source nibble should be corrected to PLAYBACK_DEVICE_1 (4)";
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test sendToAsync with header construction — sendToAsync prepends header: src=4, dst=TV(0) => 0x40
TEST_F(ConnectionTest, SendToAsyncHeaderConstruction) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<unsigned char> capturedBuf;
    bool txCalled = false;

    EXPECT_CALL(*mock, HdmiCecTxAsync(_, _, _))
        .Times(1)
        .WillOnce(Invoke([&](int, const unsigned char* buf, int len) -> int {
            std::lock_guard<std::mutex> lk(mtx);
            capturedBuf.assign(buf, buf + len);
            txCalled = true;
            cv.notify_one();
            return HDMI_CEC_IO_SUCCESS;
        }));

    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();

    CECFrame frame;
    frame.append(0x83); // Active Source
    frame.append(0x10);
    frame.append(0x00);

    EXPECT_NO_THROW(conn.sendToAsync(LogicalAddress::TV, frame));

    {
        std::unique_lock<std::mutex> lk(mtx);
        ASSERT_TRUE(cv.wait_for(lk, std::chrono::milliseconds(500), [&]{ return txCalled; }))
            << "HdmiCecTxAsync was not called within timeout";
    }
    conn.close();

    // Header prepended: src=4 (PLAYBACK_DEVICE_1), dst=0 (TV) => 0x40
    ASSERT_GE(capturedBuf.size(), (size_t)1);
    EXPECT_EQ(capturedBuf[0], 0x40);
    EXPECT_EQ(capturedBuf[1], 0x83);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// Test connection close clears all listeners
TEST_F(ConnectionTest, CloseRemovesAllListeners) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    TestFrameListener listener1;
    TestFrameListener listener2;
    
    conn.addFrameListener(&listener1);
    conn.addFrameListener(&listener2);
    
    // Close should clear listeners
    EXPECT_NO_THROW(conn.close());
    
    // Should be able to reopen and add new listeners
    EXPECT_NO_THROW(conn.open());
    EXPECT_NO_THROW(conn.addFrameListener(&listener1));
    EXPECT_NO_THROW(conn.removeFrameListener(&listener1));
    
    conn.close();
}

// Test large frame with matchSource — verify full frame reaches driver and source nibble is correct
TEST_F(ConnectionTest, LargeFrameMatchSource) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
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
    frame.append(0x4F); // src=4, dst=F
    for (int i = 0; i < 14; i++) {
        frame.append(0x00);
    }

    EXPECT_NO_THROW(conn.send(frame, 100));
    conn.close();

    ASSERT_EQ(capturedBuf.size(), (size_t)15);
    EXPECT_EQ((capturedBuf[0] >> 4) & 0x0F, 0x4) << "Source nibble must be PLAYBACK_DEVICE_1 (4)";
    EXPECT_EQ(capturedBuf[0] & 0x0F, 0xF)        << "Destination nibble must be F";
    ::testing::Mock::VerifyAndClearExpectations(mock);
}
    
    conn.close();
}

// Test getSource returns correct value
TEST_F(ConnectionTest, GetSourceReturnsCorrectValue) {
    Connection conn(LogicalAddress::RECORDING_DEVICE_1, false);
    EXPECT_EQ(conn.getSource().toInt(), LogicalAddress::RECORDING_DEVICE_1);
}

// Test setSource updates logical address
TEST_F(ConnectionTest, SetSourceUpdatesAddress) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    EXPECT_EQ(conn.getSource().toInt(), LogicalAddress::PLAYBACK_DEVICE_1);
    
    conn.setSource(LogicalAddress::PLAYBACK_DEVICE_2);
    EXPECT_EQ(conn.getSource().toInt(), LogicalAddress::PLAYBACK_DEVICE_2);
}

// Test connection with all different logical addresses
TEST_F(ConnectionTest, AllLogicalAddresses) {
    int addresses[] = {
        LogicalAddress::TV,
        LogicalAddress::RECORDING_DEVICE_1,
        LogicalAddress::RECORDING_DEVICE_2,
        LogicalAddress::TUNER_1,
        LogicalAddress::PLAYBACK_DEVICE_1,
        LogicalAddress::AUDIO_SYSTEM,
        LogicalAddress::TUNER_2,
        LogicalAddress::TUNER_3,
        LogicalAddress::PLAYBACK_DEVICE_2,
        LogicalAddress::RECORDING_DEVICE_3,
        LogicalAddress::TUNER_4,
        LogicalAddress::PLAYBACK_DEVICE_3,
        LogicalAddress::UNREGISTERED,
        LogicalAddress::BROADCAST
    };
    
    for (int addr : addresses) {
        Connection conn(LogicalAddress(addr), false);
        EXPECT_EQ(conn.getSource().toInt(), addr);
    }
}

// Test rapid open/close cycles
TEST_F(ConnectionTest, RapidOpenCloseCycles) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    
    for (int i = 0; i < 20; i++) {
        EXPECT_NO_THROW(conn.open());
        EXPECT_NO_THROW(conn.close());
    }
}

// Test concurrent listener add/remove operations
TEST_F(ConnectionTest, ConcurrentListenerOperations) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    TestFrameListener listeners[10];
    
    // Add all listeners
    for (int i = 0; i < 10; i++) {
        EXPECT_NO_THROW(conn.addFrameListener(&listeners[i]));
    }
    
    // Remove all listeners
    for (int i = 0; i < 10; i++) {
        EXPECT_NO_THROW(conn.removeFrameListener(&listeners[i]));
    }
    
    conn.close();
}
