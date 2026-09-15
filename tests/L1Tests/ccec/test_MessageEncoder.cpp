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

/*
 * ============= Message types no encode test had reached =============
 *
 * MessageEncoder::encode() is a template that calls msg.opCode() and msg.serialize(frame), so a
 * message class whose type is never handed to the encoder has both of those bodies sitting
 * uncovered no matter how thoroughly the operands inside it are tested.  That is what left
 * ccec/include/ccec/Messages.hpp at a measured 76.5% (228/298) in the baseline this file's cases
 * were written against - the lowest figure in the middleware trace after the two even smaller
 * headers - while every .cpp in ccec/src was at or above the bar.  With the cases below in place
 * it measures 92.5% (294/318).  Re-measure with ../run_coverage.sh rather than trusting either
 * figure as current.
 *
 * The cases below encode the message types that were in that position.  Each asserts the exact
 * bytes rather than only the length, because the byte layout IS the message: an operand
 * serialised in the wrong order, or an opCode() that returns the wrong constant, produces a
 * frame of the right size that no CEC peer will act on.  Where a message has a documented
 * "unset" form - SystemAudioModeRequest and RequestCurrentLatency both take the broadcast
 * physical address 0xF.0xF.0xF.0xF as their default and then serialise NO operand at all - both
 * arms are covered, because the empty arm is the one a happy-path test never takes.
 */

// SET_MENU_LANGUAGE carries a 3-character ISO-639 language code.
TEST_F(MessageEncoderTest, EncodeSetMenuLanguage) {
    SetMenuLanguage msg(Language("eng"));
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 4u);                             // opcode + 3 language bytes
    EXPECT_EQ(buf[0], (uint8_t)SET_MENU_LANGUAGE);  // 0x32
    EXPECT_EQ(buf[1], (uint8_t)'e');
    EXPECT_EQ(buf[2], (uint8_t)'n');
    EXPECT_EQ(buf[3], (uint8_t)'g');
}

// SET_OSD_NAME carries the device's display name, one byte per character and no terminator.
TEST_F(MessageEncoderTest, EncodeSetOSDName) {
    SetOSDName msg(OSDName("TV"));
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 3u);
    EXPECT_EQ(buf[0], (uint8_t)SET_OSD_NAME);       // 0x47
    EXPECT_EQ(buf[1], (uint8_t)'T');
    EXPECT_EQ(buf[2], (uint8_t)'V');
}

// SET_OSD_STRING carries a free-text string for the sink to display.
TEST_F(MessageEncoderTest, EncodeSetOSDString) {
    SetOSDString msg(OSDString("Hi"));
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 3u);
    EXPECT_EQ(buf[0], (uint8_t)SET_OSD_STRING);     // 0x64
    EXPECT_EQ(buf[1], (uint8_t)'H');
    EXPECT_EQ(buf[2], (uint8_t)'i');
}

// DEVICE_VENDOR_ID carries a 3-byte IEEE OUI, most significant byte first.
TEST_F(MessageEncoderTest, EncodeDeviceVendorID) {
    DeviceVendorID msg(VendorID(0x01, 0x02, 0x03));
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 4u);
    EXPECT_EQ(buf[0], (uint8_t)DEVICE_VENDOR_ID);   // 0x87
    EXPECT_EQ(buf[1], 0x01u);
    EXPECT_EQ(buf[2], 0x02u);
    EXPECT_EQ(buf[3], 0x03u);
}

// FEATURE_ABORT carries the rejected opcode followed by the reason, in that order.  The order
// is the point of the assertion: reversed, the peer reads the reason as the feature.
TEST_F(MessageEncoderTest, EncodeFeatureAbort) {
    FeatureAbort msg(OpCode(GIVE_DEVICE_POWER_STATUS),
                     AbortReason(AbortReason::UNRECOGNIZED_OPCODE));
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 3u);
    EXPECT_EQ(buf[0], (uint8_t)FEATURE_ABORT);              // 0x00
    EXPECT_EQ(buf[1], (uint8_t)GIVE_DEVICE_POWER_STATUS);   // the aborted feature
    EXPECT_EQ(buf[2], (uint8_t)AbortReason::UNRECOGNIZED_OPCODE);
}

// ROUTING_CHANGE carries two physical addresses: the original, then the new one.
TEST_F(MessageEncoderTest, EncodeRoutingChange) {
    RoutingChange msg(PhysicalAddress(1, 0, 0, 0), PhysicalAddress(2, 0, 0, 0));
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 5u);                             // opcode + 2 + 2
    EXPECT_EQ(buf[0], (uint8_t)ROUTING_CHANGE);     // 0x80
    EXPECT_EQ(buf[1], 0x10u);                       // from 1.0.0.0
    EXPECT_EQ(buf[2], 0x00u);
    EXPECT_EQ(buf[3], 0x20u);                       // to 2.0.0.0
    EXPECT_EQ(buf[4], 0x00u);
}

// ROUTING_INFORMATION carries the single physical address now being routed to.
TEST_F(MessageEncoderTest, EncodeRoutingInformation) {
    RoutingInformation msg(PhysicalAddress(3, 1, 0, 0));
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 3u);
    EXPECT_EQ(buf[0], (uint8_t)ROUTING_INFORMATION); // 0x81
    EXPECT_EQ(buf[1], 0x31u);                        // (3<<4)|1
    EXPECT_EQ(buf[2], 0x00u);
}

// SET_STREAM_PATH carries the physical address whose stream the sink should select.
TEST_F(MessageEncoderTest, EncodeSetStreamPath) {
    SetStreamPath msg(PhysicalAddress(2, 2, 0, 0));
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 3u);
    EXPECT_EQ(buf[0], (uint8_t)SET_STREAM_PATH);    // 0x86
    EXPECT_EQ(buf[1], 0x22u);
    EXPECT_EQ(buf[2], 0x00u);
}

// SET_SYSTEM_AUDIO_MODE carries the on/off status as one byte.
TEST_F(MessageEncoderTest, EncodeSetSystemAudioMode) {
    SetSystemAudioMode msg(SystemAudioStatus(SystemAudioStatus::ON));
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 2u);
    EXPECT_EQ(buf[0], (uint8_t)SET_SYSTEM_AUDIO_MODE);  // 0x72
    EXPECT_EQ(buf[1], (uint8_t)SystemAudioStatus::ON);
}

// REPORT_AUDIO_STATUS packs the mute flag into bit 7 and the volume into bits 0-6.
TEST_F(MessageEncoderTest, EncodeReportAudioStatus) {
    ReportAudioStatus msg(AudioStatus(0x32));
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 2u);
    EXPECT_EQ(buf[0], (uint8_t)REPORT_AUDIO_STATUS);   // 0x7A
    EXPECT_EQ(buf[1], 0x32u);
}

// SYSTEM_AUDIO_MODE_REQUEST with a real address serialises it; see the next case for the
// default form, which deliberately serialises nothing.
TEST_F(MessageEncoderTest, EncodeSystemAudioModeRequestWithAnAddress) {
    SystemAudioModeRequest msg(PhysicalAddress(1, 1, 0, 0));
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 3u);
    EXPECT_EQ(buf[0], (uint8_t)SYSTEM_AUDIO_MODE_REQUEST);  // 0x70
    EXPECT_EQ(buf[1], 0x11u);
    EXPECT_EQ(buf[2], 0x00u);
}

// The default-constructed form means "turn system audio mode on for whatever is active": the
// address is 0xF.0xF.0xF.0xF and serialize() returns the frame UNTOUCHED, so the encoded
// message is the opcode alone.  This is the arm a happy-path test never takes.
TEST_F(MessageEncoderTest, EncodeSystemAudioModeRequestWithoutAnAddressEmitsOpcodeOnly) {
    SystemAudioModeRequest msg;
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 1u);
    EXPECT_EQ(buf[0], (uint8_t)SYSTEM_AUDIO_MODE_REQUEST);
}

// REQUEST_CURRENT_LATENCY has the same two arms for the same reason.
TEST_F(MessageEncoderTest, EncodeRequestCurrentLatencyWithAnAddress) {
    RequestCurrentLatency msg(PhysicalAddress(1, 2, 0, 0));
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 3u);
    EXPECT_EQ(buf[0], (uint8_t)REQUEST_CURRENT_LATENCY);  // 0xA7
    EXPECT_EQ(buf[1], 0x12u);
    EXPECT_EQ(buf[2], 0x00u);
}

TEST_F(MessageEncoderTest, EncodeRequestCurrentLatencyWithoutAnAddressEmitsOpcodeOnly) {
    RequestCurrentLatency msg;
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 1u);
    EXPECT_EQ(buf[0], (uint8_t)REQUEST_CURRENT_LATENCY);
}

// REPORT_CURRENT_LATENCY carries the physical address then video latency and latency flags.
// With flags & 0x3 != 3 the audio-output-delay byte is omitted, which is the shorter of the
// two forms the constructor can build.
TEST_F(MessageEncoderTest, EncodeReportCurrentLatencyWithoutAudioDelay) {
    ReportCurrentLatency msg(PhysicalAddress(1, 0, 0, 0), 0x0A, 0x00);
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 5u);                                  // opcode + 2 + video + flags
    EXPECT_EQ(buf[0], (uint8_t)REPORT_CURRENT_LATENCY);  // 0xA8
    EXPECT_EQ(buf[1], 0x10u);
    EXPECT_EQ(buf[2], 0x00u);
    EXPECT_EQ(buf[3], 0x0Au);
    EXPECT_EQ(buf[4], 0x00u);
}

// With flags & 0x3 == 3 the constructor appends the audio output delay, so the encoded frame
// is one byte longer.  Both arms of that `if` are now covered.
TEST_F(MessageEncoderTest, EncodeReportCurrentLatencyWithAudioDelay) {
    ReportCurrentLatency msg(PhysicalAddress(1, 0, 0, 0), 0x0A, 0x03, 0x14);
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 6u);
    EXPECT_EQ(buf[0], (uint8_t)REPORT_CURRENT_LATENCY);
    EXPECT_EQ(buf[1], 0x10u);
    EXPECT_EQ(buf[2], 0x00u);
    EXPECT_EQ(buf[3], 0x0Au);
    EXPECT_EQ(buf[4], 0x03u);
    EXPECT_EQ(buf[5], 0x14u);
}

// USER_CONTROL_PRESSED carries the single UI command code.
TEST_F(MessageEncoderTest, EncodeUserControlPressed) {
    UserControlPressed msg(UICommand(UICommand::UI_COMMAND_UP));
    CECFrame frame = encoder.encode(msg);
    const uint8_t *buf; size_t len;
    getBuf(frame, &buf, &len);
    ASSERT_EQ(len, 2u);
    EXPECT_EQ(buf[0], (uint8_t)USER_CONTROL_PRESSED);  // 0x44
    EXPECT_EQ(buf[1], (uint8_t)UICommand::UI_COMMAND_UP);
}
