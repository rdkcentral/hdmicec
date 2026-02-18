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
#include "ccec/Connection.hpp"

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
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36); // Standby
    
    EXPECT_NO_THROW(conn.send(frame, 100));
    
    conn.close();
}

// Test send with zero timeout
TEST_F(ConnectionTest, SendWithZeroTimeout) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x83);
    
    EXPECT_NO_THROW(conn.send(frame, 0));
    
    conn.close();
}

// Test send with default timeout
TEST_F(ConnectionTest, SendWithDefaultTimeout) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);
    
    EXPECT_NO_THROW(conn.send(frame));
    
    conn.close();
}

// Test send with throw parameter
TEST_F(ConnectionTest, SendWithThrowParameter) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);
    
    EXPECT_NO_THROW(conn.send(frame, 100, Throw_e()));
    
    conn.close();
}

// Test sendTo with specific address
TEST_F(ConnectionTest, SendToSpecificAddress) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x83); // Give Device Power Status
    
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame));
    
    conn.close();
}

// Test sendTo with timeout
TEST_F(ConnectionTest, SendToWithTimeout) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x83);
    
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame, 200));
    
    conn.close();
}

// Test sendTo with throw parameter
TEST_F(ConnectionTest, SendToWithThrowParameter) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x83);
    
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame, 100, Throw_e()));
    
    conn.close();
}

// Test sendToAsync
TEST_F(ConnectionTest, SendToAsync) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x36); // Standby
    
    EXPECT_NO_THROW(conn.sendToAsync(LogicalAddress::TV, frame));
    
    usleep(50000); // Give async time to process
    
    conn.close();
}

// Test sendAsync
TEST_F(ConnectionTest, SendAsync) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x36);
    
    EXPECT_NO_THROW(conn.sendAsync(frame));
    
    usleep(50000);
    
    conn.close();
}

// Test poll
TEST_F(ConnectionTest, PollAddress) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    EXPECT_NO_THROW(conn.poll(LogicalAddress::PLAYBACK_DEVICE_1, Throw_e()));
    
    conn.close();
}

// Test ping
TEST_F(ConnectionTest, PingAddress) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    EXPECT_NO_THROW(conn.ping(LogicalAddress::PLAYBACK_DEVICE_1, LogicalAddress::TV, Throw_e()));
    
    conn.close();
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

// Test broadcast message
TEST_F(ConnectionTest, SendBroadcast) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x82); // Active Source
    frame.append(0x10);
    frame.append(0x00);
    
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::BROADCAST, frame));
    
    conn.close();
}

// Test sending to same address
TEST_F(ConnectionTest, SendToSameAddress) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x83);
    
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::PLAYBACK_DEVICE_1, frame));
    
    conn.close();
}

// Test multiple sends
TEST_F(ConnectionTest, MultipleSends) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame1;
    frame1.append(0x83);
    
    CECFrame frame2;
    frame2.append(0x36);
    
    CECFrame frame3;
    frame3.append(0x8F);
    
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame1));
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame2));
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame3));
    
    conn.close();
}

// Test send with different addresses
TEST_F(ConnectionTest, SendToDifferentAddresses) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x83);
    
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame));
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::AUDIO_SYSTEM, frame));
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::RECORDING_DEVICE_1, frame));
    
    conn.close();
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

// Test sendAsync multiple times
TEST_F(ConnectionTest, MultipleAsyncSends) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    for (int i = 0; i < 5; i++) {
        CECFrame frame;
        frame.append(0x36);
        EXPECT_NO_THROW(conn.sendAsync(frame));
    }
    
    usleep(100000); // Give async time to process
    
    conn.close();
}

// Test sendToAsync to different addresses
TEST_F(ConnectionTest, AsyncSendToDifferentAddresses) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x83);
    
    EXPECT_NO_THROW(conn.sendToAsync(LogicalAddress::TV, frame));
    EXPECT_NO_THROW(conn.sendToAsync(LogicalAddress::AUDIO_SYSTEM, frame));
    EXPECT_NO_THROW(conn.sendToAsync(LogicalAddress::BROADCAST, frame));
    
    usleep(100000);
    
    conn.close();
}

// Test empty frame send
TEST_F(ConnectionTest, SendEmptyFrame) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame emptyFrame;
    EXPECT_NO_THROW(conn.send(emptyFrame));
    
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

// Test with various timeout values
TEST_F(ConnectionTest, VariousTimeouts) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x36);
    
    EXPECT_NO_THROW(conn.send(frame, 0));
    EXPECT_NO_THROW(conn.send(frame, 50));
    EXPECT_NO_THROW(conn.send(frame, 100));
    EXPECT_NO_THROW(conn.send(frame, 250));
    EXPECT_NO_THROW(conn.send(frame, 500));
    
    conn.close();
}

// Test sendTo with various timeout values
TEST_F(ConnectionTest, SendToVariousTimeouts) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x83);
    
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame, 0));
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame, 100));
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame, 500));
    
    conn.close();
}

// Test connection remains functional after errors
TEST_F(ConnectionTest, ConnectionAfterSend) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x36);
    
    // Send multiple times to ensure connection stays functional
    for (int i = 0; i < 10; i++) {
        EXPECT_NO_THROW(conn.send(frame, 0));
    }
    
    conn.close();
}

// Test mixed sync and async operations
TEST_F(ConnectionTest, MixedSyncAsync) {
    Connection conn(LogicalAddress::PLAYBACK_DEVICE_1, false);
    conn.open();
    
    CECFrame frame;
    frame.append(0x36);
    
    EXPECT_NO_THROW(conn.send(frame, 0));
    EXPECT_NO_THROW(conn.sendAsync(frame));
    EXPECT_NO_THROW(conn.sendTo(LogicalAddress::TV, frame));
    EXPECT_NO_THROW(conn.sendToAsync(LogicalAddress::TV, frame));
    
    usleep(50000);
    
    conn.close();
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
