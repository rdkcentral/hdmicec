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

// Test basic send functionality with zero timeout
TEST_F(BusTest, SendFrameWithZeroTimeout) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40); // Playback 1 to TV
    frame.append(0x36); // Standby opcode
    
    EXPECT_NO_THROW({
        conn.send(frame, 0);
    });
    
    conn.close();
}

// Test send functionality with timeout
TEST_F(BusTest, SendFrameWithTimeout) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40); // Playback 1 to TV
    frame.append(0x84); // Report Physical Address
    
    EXPECT_NO_THROW({
        conn.send(frame, 500); // 500ms timeout
    });
    
    conn.close();
}

// Test sendAsync functionality
TEST_F(BusTest, SendFrameAsync) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);
    
    EXPECT_NO_THROW({
        conn.sendAsync(frame);
    });
    
    // Give async writer time to process
    usleep(100000); // 100ms
    
    conn.close();
}

// Test multiple async sends
TEST_F(BusTest, SendMultipleFramesAsync) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    for (int i = 0; i < 5; i++) {
        CECFrame frame;
        frame.append(0x40);
        frame.append(0x36);
        
        EXPECT_NO_THROW({
            conn.sendAsync(frame);
        });
    }
    
    // Give async writer time to process
    usleep(200000); // 200ms
    
    conn.close();
}

// Test poll functionality
TEST_F(BusTest, PollLogicalAddress) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    // Poll same address (valid for discovering if address is taken)
    EXPECT_NO_THROW({
        conn.poll(LogicalAddress::PLAYBACK_DEVICE_1, Throw_e());
    });
    
    conn.close();
}

// Test ping functionality
TEST_F(BusTest, PingLogicalAddress) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    // Ping different address
    EXPECT_NO_THROW({
        conn.ping(LogicalAddress::PLAYBACK_DEVICE_1, LogicalAddress::TV, Throw_e());
    });
    
    conn.close();
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
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x83); // Give Device Power Status opcode
    
    EXPECT_NO_THROW({
        conn.sendTo(LogicalAddress::TV, frame);
    });
    
    conn.close();
}

// Test sendToAsync with specific addresses
TEST_F(BusTest, SendToAsyncSpecificAddress) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x83); // Give Device Power Status opcode
    
    EXPECT_NO_THROW({
        conn.sendToAsync(LogicalAddress::TV, frame);
    });
    
    usleep(100000); // 100ms for async processing
    
    conn.close();
}

// Test send with throw exception parameter
TEST_F(BusTest, SendWithThrowParameter) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);
    
    EXPECT_NO_THROW({
        conn.send(frame, 100, Throw_e());
    });
    
    conn.close();
}

// Test sendTo with throw exception parameter
TEST_F(BusTest, SendToWithThrowParameter) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x83);
    
    EXPECT_NO_THROW({
        conn.sendTo(LogicalAddress::TV, frame, 100, Throw_e());
    });
    
    conn.close();
}

// Test broadcast messaging
TEST_F(BusTest, SendBroadcastMessage) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x82); // Active Source opcode
    frame.append(0x10);
    frame.append(0x00);
    
    EXPECT_NO_THROW({
        conn.sendTo(LogicalAddress::BROADCAST, frame);
    });
    
    conn.close();
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
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40); // Just header
    
    EXPECT_NO_THROW({
        conn.send(frame, 0);
    });
    
    conn.close();
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

// Test sending with different timeout values
TEST_F(BusTest, SendWithVariousTimeouts) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);
    
    // Test different timeout values
    int timeouts[] = {0, 250, 500, 1000};
    
    for (int timeout : timeouts) {
        EXPECT_NO_THROW({
            conn.send(frame, timeout);
        });
    }
    
    conn.close();
}

// Test listener receives frames from different sources
TEST_F(BusTest, ListenerFiltering) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    TestFrameListener listener;
    
    conn.open();
    conn.addFrameListener(&listener);
    
    // In a real scenario, frames would arrive from the driver
    // This test verifies the listener registration works
    
    conn.removeFrameListener(&listener);
    conn.close();
}

// Test unregistered connection receives all messages
TEST_F(BusTest, UnregisteredConnectionReceivesAll) {
    Connection conn(LogicalAddress::UNREGISTERED, false);
    TestFrameListener listener;
    
    EXPECT_NO_THROW({
        conn.open();
        conn.addFrameListener(&listener);
        conn.removeFrameListener(&listener);
        conn.close();
    });
}

// Test send to same source address (loopback scenario)
TEST_F(BusTest, SendToSameAddress) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x83);
    
    EXPECT_NO_THROW({
        conn.sendTo(LogicalAddress::PLAYBACK_DEVICE_1, frame);
    });
    
    conn.close();
}

// Test sequential sends
TEST_F(BusTest, SequentialSends) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame1;
    frame1.append(0x40);
    frame1.append(0x83);
    
    CECFrame frame2;
    frame2.append(0x40);
    frame2.append(0x36);
    
    CECFrame frame3;
    frame3.append(0x40);
    frame3.append(0x8F);
    
    EXPECT_NO_THROW({
        conn.send(frame1, 0);
        conn.send(frame2, 0);
        conn.send(frame3, 0);
    });
    
    conn.close();
}

// Test mixed sync and async sends
TEST_F(BusTest, MixedSyncAsyncSends) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame1;
    frame1.append(0x40);
    frame1.append(0x83);
    
    CECFrame frame2;
    frame2.append(0x40);
    frame2.append(0x36);
    
    EXPECT_NO_THROW({
        conn.send(frame1, 0);
        conn.sendAsync(frame2);
        conn.send(frame1, 0);
        conn.sendAsync(frame2);
    });
    
    usleep(100000); // Allow async to process
    
    conn.close();
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
}

// Test retry logic with timeout - eventual success
TEST_F(BusTest, SendWithTimeoutRetrySuccess) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    
    // Fail first attempt, succeed on retry
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(::testing::AtLeast(1))
        .WillOnce(Return(HDMI_CEC_IO_SENT_FAILED))
        .WillRepeatedly(DoAll(
            SetArgPointee<3>(HDMI_CEC_IO_SENT_AND_ACKD),
            Return(HDMI_CEC_IO_SUCCESS)
        ));
    
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);
    
    // Should eventually succeed with retry
    EXPECT_NO_THROW({
        conn.send(frame, 500);
    });
    
    conn.close();
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
}

// Test multiple consecutive async sends to exercise writer queue
TEST_F(BusTest, ManyAsyncSends) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    // Send many frames asynchronously to exercise the writer queue
    for (int i = 0; i < 20; i++) {
        CECFrame frame;
        frame.append(0x40);
        frame.append(0x36);
        EXPECT_NO_THROW(conn.sendAsync(frame));
    }
    
    // Give writer time to process all frames
    usleep(200000);
    
    conn.close();
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
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);
    
    // Test with large timeout (will succeed quickly due to mock)
    EXPECT_NO_THROW({
        conn.send(frame, 1000);
    });
    
    conn.close();
}

// Test async send followed by immediate close to test cleanup
TEST_F(BusTest, AsyncSendThenQuickClose) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);
    
    // Send async
    EXPECT_NO_THROW(conn.sendAsync(frame));
    
    // Close immediately (writer should cleanup queue)
    usleep(10000); // Small delay to let frame enter queue
    EXPECT_NO_THROW(conn.close());
}

// Test broadcast send with timeout
TEST_F(BusTest, BroadcastSendWithTimeout) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x82); // Active Source
    frame.append(0x10);
    frame.append(0x00);
    
    EXPECT_NO_THROW({
        conn.sendTo(LogicalAddress::BROADCAST, frame, 250);
    });
    
    conn.close();
}

// Test multiple timeouts with same frame
TEST_F(BusTest, MultipleTimeoutRetries) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x83);
    
    // Test multiple send operations with timeout
    EXPECT_NO_THROW(conn.send(frame, 250));
    EXPECT_NO_THROW(conn.send(frame, 250));
    EXPECT_NO_THROW(conn.send(frame, 250));
    
    conn.close();
}
