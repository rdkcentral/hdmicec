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
#include "ccec/Header.hpp"



class MessageEncoderTest : public ::testing::Test {
protected:
    MessageEncoder encoder;
};

// Helper to extract the raw buffer from a CECFrame
static void getBuf(const CECFrame &frame, const uint8_t **buf, size_t *len) {
    frame.getBuffer(buf, len);
}

// IMAGE_VIEW_ON has no operands — encoded frame is exactly 1 byte: opcode only.
TEST_F(MessageEncoderTest, EncodeImageViewOn) {
    ImageViewOn msg;
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 1u);
    EXPECT_EQ(buf[0], (uint8_t)IMAGE_VIEW_ON); // 0x04
}

// TEXT_VIEW_ON has no operands — exactly 1 byte.
TEST_F(MessageEncoderTest, EncodeTextViewOn) {
    TextViewOn msg;
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 1u);
    EXPECT_EQ(buf[0], (uint8_t)TEXT_VIEW_ON); // 0x0D
}

// ACTIVE_SOURCE carries a 2-byte physical address (4 nibbles packed into 2 bytes).
// PhysicalAddress(1,0,0,0): byte1 = (1<<4)|0 = 0x10, byte2 = (0<<4)|0 = 0x00
TEST_F(MessageEncoderTest, EncodeActiveSource) {
    PhysicalAddress phy(1, 0, 0, 0);
    ActiveSource msg(phy);
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 3u);                          // opcode + 2-byte physical address
    EXPECT_EQ(buf[0], (uint8_t)ACTIVE_SOURCE);  // 0x82
    EXPECT_EQ(buf[1], 0x10u);                    // (1<<4)|0
    EXPECT_EQ(buf[2], 0x00u);                    // (0<<4)|0
}

// STANDBY has no operands.
TEST_F(MessageEncoderTest, EncodeStandby) {
    Standby msg;
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 1u);
    EXPECT_EQ(buf[0], (uint8_t)STANDBY); // 0x36
}

// INACTIVE_SOURCE: opcode + 2-byte physical address
// PhysicalAddress(2,1,0,0): byte1=(2<<4)|1=0x21, byte2=(0<<4)|0=0x00
TEST_F(MessageEncoderTest, EncodeInActiveSource) {
    PhysicalAddress phy(2, 1, 0, 0);
    InActiveSource msg(phy);
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 3u);
    EXPECT_EQ(buf[0], (uint8_t)INACTIVE_SOURCE); // 0x9D
    EXPECT_EQ(buf[1], 0x21u);                      // (2<<4)|1
    EXPECT_EQ(buf[2], 0x00u);                      // (0<<4)|0
}

// CEC_VERSION: opcode + 1-byte version. Version::V_1_4 = 5.
TEST_F(MessageEncoderTest, EncodeCECVersion) {
    CECVersion msg(Version::V_1_4);
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 2u);                          // opcode + 1-byte version
    EXPECT_EQ(buf[0], (uint8_t)CEC_VERSION);    // 0x9E
    EXPECT_EQ(buf[1], (uint8_t)Version::V_1_4); // 5
}

// REPORT_POWER_STATUS: opcode + 1-byte power status. PowerStatus::ON = 0.
TEST_F(MessageEncoderTest, EncodeReportPowerStatus) {
    ReportPowerStatus msg(PowerStatus::ON);
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 2u);
    EXPECT_EQ(buf[0], (uint8_t)REPORT_POWER_STATUS);  // 0x90
    EXPECT_EQ(buf[1], (uint8_t)PowerStatus::ON);       // 0
}

// REPORT_PHYSICAL_ADDRESS: opcode + 2-byte physical address + 1-byte device type
// PhysicalAddress(1,2,3,4): byte1=(1<<4)|2=0x12, byte2=(3<<4)|4=0x34
// DeviceType::PLAYBACK_DEVICE = 4
TEST_F(MessageEncoderTest, EncodeReportPhysicalAddress) {
    PhysicalAddress phy(1, 2, 3, 4);
    DeviceType dt(DeviceType::PLAYBACK_DEVICE);
    ReportPhysicalAddress msg(phy, dt);
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 4u);                                  // opcode + 2-byte addr + 1-byte type
    EXPECT_EQ(buf[0], (uint8_t)REPORT_PHYSICAL_ADDRESS); // 0x84
    EXPECT_EQ(buf[1], 0x12u);                             // (1<<4)|2
    EXPECT_EQ(buf[2], 0x34u);                             // (3<<4)|4
    EXPECT_EQ(buf[3], (uint8_t)DeviceType::PLAYBACK_DEVICE); // 4
}

// Encode with Header: header byte is prepended before the opcode.
// Header(PLAYBACK_DEVICE_1, TV): from=4, to=0 -> (4<<4)|0 = 0x40
TEST_F(MessageEncoderTest, EncodeWithHeader) {
    Header hdr(LogicalAddress::PLAYBACK_DEVICE_1, LogicalAddress::TV);
    Standby msg;
    CECFrame frame = encoder.encode(hdr, msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 2u);
    EXPECT_EQ(buf[0], 0x40u);                 // header: src=4 (PB1), dst=0 (TV)
    EXPECT_EQ(buf[1], (uint8_t)STANDBY);     // 0x36
}

// Round-trip: encode then re-parse the physical address from the raw bytes.
TEST_F(MessageEncoderTest, EncodeActiveSsourceRoundTrip) {
    PhysicalAddress original(3, 2, 1, 0);
    ActiveSource msg(original);
    CECFrame frame = encoder.encode(msg);
    // Re-parse from the frame (skip opcode byte at index 0)
    ActiveSource decoded(frame, 1);
    EXPECT_STREQ(decoded.physicalAddress.toString().c_str(), "3.2.1.0");
}
