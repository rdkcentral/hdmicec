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

using namespace CCEC;

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
    EXPECT_EQ(frame.length(), 0);
}

TEST_F(CECFrameTest, ConstructorWithHeader) {
    Header header(LogicalAddress::TV, LogicalAddress::PLAYBACK_DEVICE_1);
    CECFrame frame(header);
    EXPECT_GT(frame.length(), 0);
}

TEST_F(CECFrameTest, CopyConstructor) {
    Header header(LogicalAddress::TV, LogicalAddress::PLAYBACK_DEVICE_1);
    CECFrame frame1(header);
    CECFrame frame2(frame1);
    EXPECT_EQ(frame1.length(), frame2.length());
}

TEST_F(CECFrameTest, HexDumpOutput) {
    Header header(LogicalAddress::TV, LogicalAddress::PLAYBACK_DEVICE_1);
    CECFrame frame(header);
    EXPECT_NO_THROW(frame.hexDump());
}
