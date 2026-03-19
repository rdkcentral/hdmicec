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
#include "ccec/MessageEncoder.hpp"
#include "ccec/Messages.hpp"

using namespace CCEC;

class MessageEncoderTest : public ::testing::Test {
protected:
    MessageEncoder encoder;
};

TEST_F(MessageEncoderTest, EncodeImageViewOn) {
    ImageViewOn msg;
    EXPECT_NO_THROW({
        CECFrame frame = encoder.encode(msg);
        EXPECT_GT(frame.length(), 0);
    });
}

TEST_F(MessageEncoderTest, EncodeTextViewOn) {
    TextViewOn msg;
    EXPECT_NO_THROW({
        CECFrame frame = encoder.encode(msg);
        EXPECT_GT(frame.length(), 0);
    });
}

TEST_F(MessageEncoderTest, EncodeActiveSource) {
    PhysicalAddress phy(1, 0, 0, 0);
    ActiveSource msg(phy);
    EXPECT_NO_THROW({
        CECFrame frame = encoder.encode(msg);
        EXPECT_GT(frame.length(), 0);
    });
}
