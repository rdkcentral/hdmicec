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
