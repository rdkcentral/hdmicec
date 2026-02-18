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
#include "ccec/CECFrame.hpp"
#include "ccec/Header.hpp"

class CECFrameTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup code before each test
    }

    void TearDown() override {
        // Cleanup code after each test
    }
};

TEST_F(CECFrameTest, DefaultConstructor) {
    CECFrame frame;
    EXPECT_EQ(frame.length(), (size_t)0);
}

TEST_F(CECFrameTest, ConstructorWithHeader) {
    Header header(LogicalAddress::TV, LogicalAddress::PLAYBACK_DEVICE_1);
    CECFrame frame;
    header.serialize(frame);
    EXPECT_GT(frame.length(), (size_t)0);
}

TEST_F(CECFrameTest, CopyConstructor) {
    Header header(LogicalAddress::TV, LogicalAddress::PLAYBACK_DEVICE_1);
    CECFrame frame1;
    header.serialize(frame1);
    CECFrame frame2(frame1);
    EXPECT_EQ(frame1.length(), frame2.length());
}

TEST_F(CECFrameTest, HexDumpOutput) {
    Header header(LogicalAddress::TV, LogicalAddress::PLAYBACK_DEVICE_1);
    CECFrame frame;
    header.serialize(frame);
    EXPECT_NO_THROW(frame.hexDump());
}

TEST_F(CECFrameTest, ConstructorWithBuffer) {
    uint8_t buffer[] = {0x40, 0x83, 0x10, 0x00};
    CECFrame frame(buffer, 4);
    EXPECT_EQ(frame.length(), (size_t)4);
    EXPECT_EQ(frame.at(0), 0x40);
    EXPECT_EQ(frame.at(1), 0x83);
}

TEST_F(CECFrameTest, ConstructorWithNullBuffer) {
    CECFrame frame(nullptr, 0);
    EXPECT_EQ(frame.length(), (size_t)0);
}

TEST_F(CECFrameTest, AppendSingleByte) {
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x83);
    EXPECT_EQ(frame.length(), (size_t)2);
    EXPECT_EQ(frame.at(0), 0x40);
    EXPECT_EQ(frame.at(1), 0x83);
}

TEST_F(CECFrameTest, AppendBuffer) {
    CECFrame frame;
    uint8_t buffer[] = {0x40, 0x83, 0x10, 0x00};
    frame.append(buffer, 4);
    EXPECT_EQ(frame.length(), (size_t)4);
    EXPECT_EQ(frame.at(0), 0x40);
    EXPECT_EQ(frame.at(3), 0x00);
}

TEST_F(CECFrameTest, AppendFrame) {
    CECFrame frame1;
    frame1.append(0x40);
    frame1.append(0x83);
    
    CECFrame frame2;
    frame2.append(0x10);
    frame2.append(0x00);
    
    frame1.append(frame2);
    EXPECT_EQ(frame1.length(), (size_t)4);
    EXPECT_EQ(frame1.at(0), 0x40);
    EXPECT_EQ(frame1.at(2), 0x10);
}

TEST_F(CECFrameTest, ResetFrame) {
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x83);
    EXPECT_EQ(frame.length(), (size_t)2);
    
    frame.reset();
    EXPECT_EQ(frame.length(), (size_t)0);
}

TEST_F(CECFrameTest, GetBufferWithPointers) {
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x83);
    
    const uint8_t *buf = nullptr;
    size_t len = 0;
    frame.getBuffer(&buf, &len);
    
    EXPECT_NE(buf, nullptr);
    EXPECT_EQ(len, (size_t)2);
    EXPECT_EQ(buf[0], 0x40);
    EXPECT_EQ(buf[1], 0x83);
}

TEST_F(CECFrameTest, GetBufferDirect) {
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x83);
    
    const uint8_t *buf = frame.getBuffer();
    EXPECT_NE(buf, nullptr);
    EXPECT_EQ(buf[0], 0x40);
}

TEST_F(CECFrameTest, AtMethod) {
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x83);
    frame.append(0x10);
    
    EXPECT_EQ(frame.at(0), 0x40);
    EXPECT_EQ(frame.at(1), 0x83);
    EXPECT_EQ(frame.at(2), 0x10);
}

TEST_F(CECFrameTest, AtMethodOutOfRange) {
    CECFrame frame;
    frame.append(0x40);
    
    EXPECT_THROW(frame.at(1), std::out_of_range);
    EXPECT_THROW(frame.at(10), std::out_of_range);
}

TEST_F(CECFrameTest, OperatorBracket) {
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x83);
    
    EXPECT_EQ(frame[0], 0x40);
    EXPECT_EQ(frame[1], 0x83);
    
    // Test modification
    frame[0] = 0x50;
    EXPECT_EQ(frame[0], 0x50);
}

TEST_F(CECFrameTest, OperatorBracketOutOfRange) {
    CECFrame frame;
    frame.append(0x40);
    
    EXPECT_THROW(frame[1], std::out_of_range);
    EXPECT_THROW(frame[10], std::out_of_range);
}

TEST_F(CECFrameTest, LengthMethod) {
    CECFrame frame;
    EXPECT_EQ(frame.length(), (size_t)0);
    
    frame.append(0x40);
    EXPECT_EQ(frame.length(), (size_t)1);
    
    frame.append(0x83);
    EXPECT_EQ(frame.length(), (size_t)2);
}

TEST_F(CECFrameTest, SubFrameBasic) {
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x83);
    frame.append(0x10);
    frame.append(0x00);
    
    CECFrame sub = frame.subFrame(1, 2);
    EXPECT_EQ(sub.length(), (size_t)2);
    EXPECT_EQ(sub.at(0), 0x83);
    EXPECT_EQ(sub.at(1), 0x10);
}

TEST_F(CECFrameTest, SubFrameFromStart) {
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x83);
    frame.append(0x10);
    
    CECFrame sub = frame.subFrame(0, 2);
    EXPECT_EQ(sub.length(), (size_t)2);
    EXPECT_EQ(sub.at(0), 0x40);
    EXPECT_EQ(sub.at(1), 0x83);
}

TEST_F(CECFrameTest, SubFrameToEnd) {
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x83);
    frame.append(0x10);
    frame.append(0x00);
    
    CECFrame sub = frame.subFrame(2, 0); // len=0 means to end
    EXPECT_EQ(sub.length(), (size_t)2);
    EXPECT_EQ(sub.at(0), 0x10);
    EXPECT_EQ(sub.at(1), 0x00);
}

TEST_F(CECFrameTest, SubFrameEmpty) {
    CECFrame frame;
    frame.append(0x40);
    
    CECFrame sub = frame.subFrame(1, 0);
    EXPECT_EQ(sub.length(), (size_t)0);
}

TEST_F(CECFrameTest, AppendMaxLength) {
    CECFrame frame;
    
    // Append up to MAX_LENGTH
    for (size_t i = 0; i < CECFrame::MAX_LENGTH; i++) {
        EXPECT_NO_THROW(frame.append((uint8_t)i));
    }
    
    EXPECT_EQ(frame.length(), CECFrame::MAX_LENGTH);
    
    // Appending beyond MAX_LENGTH should throw
    EXPECT_THROW(frame.append(0xFF), std::out_of_range);
}

TEST_F(CECFrameTest, HexDumpWithDifferentLevels) {
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x83);
    
    EXPECT_NO_THROW(frame.hexDump(1));
    EXPECT_NO_THROW(frame.hexDump(3));
    EXPECT_NO_THROW(frame.hexDump(6));
}

TEST_F(CECFrameTest, HexDumpEmptyFrame) {
    CECFrame frame;
    EXPECT_NO_THROW(frame.hexDump());
}

TEST_F(CECFrameTest, MultipleAppendOperations) {
    CECFrame frame;
    
    // Append single byte
    frame.append(0x40);
    
    // Append buffer
    uint8_t buf1[] = {0x83, 0x10};
    frame.append(buf1, 2);
    
    // Append another frame
    CECFrame frame2;
    frame2.append(0x00);
    frame.append(frame2);
    
    EXPECT_EQ(frame.length(), (size_t)4);
    EXPECT_EQ(frame.at(0), 0x40);
    EXPECT_EQ(frame.at(1), 0x83);
    EXPECT_EQ(frame.at(2), 0x10);
    EXPECT_EQ(frame.at(3), 0x00);
}

TEST_F(CECFrameTest, ResetAndReuse) {
    CECFrame frame;
    
    frame.append(0x40);
    frame.append(0x83);
    EXPECT_EQ(frame.length(), (size_t)2);
    
    frame.reset();
    EXPECT_EQ(frame.length(), (size_t)0);
    
    // Reuse after reset
    frame.append(0x10);
    frame.append(0x00);
    EXPECT_EQ(frame.length(), (size_t)2);
    EXPECT_EQ(frame.at(0), 0x10);
}

TEST_F(CECFrameTest, AppendEmptyBuffer) {
    CECFrame frame;
    frame.append(0x40);
    
    uint8_t emptyBuf[] = {};
    frame.append(emptyBuf, 0);
    
    EXPECT_EQ(frame.length(), (size_t)1);
}

TEST_F(CECFrameTest, AppendEmptyFrame) {
    CECFrame frame1;
    frame1.append(0x40);
    
    CECFrame emptyFrame;
    frame1.append(emptyFrame);
    
    EXPECT_EQ(frame1.length(), (size_t)1);
    EXPECT_EQ(frame1.at(0), 0x40);
}

TEST_F(CECFrameTest, SubFrameBeyondEnd) {
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x83);
    
    // Start beyond frame length
    CECFrame sub = frame.subFrame(5, 2);
    EXPECT_EQ(sub.length(), (size_t)0);
}

TEST_F(CECFrameTest, LargeFrame) {
    CECFrame frame;
    
    // Create a large frame
    for (size_t i = 0; i < 50; i++) {
        frame.append((uint8_t)(i & 0xFF));
    }
    
    EXPECT_EQ(frame.length(), (size_t)50);
    EXPECT_EQ(frame.at(0), 0);
    EXPECT_EQ(frame.at(25), 25);
    EXPECT_EQ(frame.at(49), 49);
}

TEST_F(CECFrameTest, ModifyViaOperator) {
    CECFrame frame;
    frame.append(0x40);
    frame.append(0x83);
    frame.append(0x10);
    
    // Modify values
    frame[0] = 0x50;
    frame[1] = 0x93;
    frame[2] = 0x20;
    
    EXPECT_EQ(frame.at(0), 0x50);
    EXPECT_EQ(frame.at(1), 0x93);
    EXPECT_EQ(frame.at(2), 0x20);
}
