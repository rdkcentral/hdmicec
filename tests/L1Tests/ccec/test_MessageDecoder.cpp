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
#include "ccec/MessageDecoder.hpp"
#include "ccec/MessageProcessor.hpp"
#include "ccec/CECFrame.hpp"



class MessageDecoderTest : public ::testing::Test {
protected:
    MessageProcessor processor;
    MessageDecoder decoder{processor};
};

TEST_F(MessageDecoderTest, DecodeValidFrame) {
    // Create a simple test frame (IMAGE_VIEW_ON)
    uint8_t testData[] = {0x40, 0x04};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodePollingMessage) {
    // Polling message - single byte (header only)
    uint8_t testData[] = {0x44};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeActiveSource) {
    // ACTIVE_SOURCE (0x82) with physical address
    uint8_t testData[] = {0x4F, 0x82, 0x10, 0x00};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeInactiveSource) {
    // INACTIVE_SOURCE (0x9D) with physical address
    uint8_t testData[] = {0x40, 0x9D, 0x10, 0x00};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeTextViewOn) {
    // TEXT_VIEW_ON (0x0D)
    uint8_t testData[] = {0x40, 0x0D};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeRequestActiveSource) {
    // REQUEST_ACTIVE_SOURCE (0x85)
    uint8_t testData[] = {0x4F, 0x85};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeStandby) {
    // STANDBY (0x36)
    uint8_t testData[] = {0x40, 0x36};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeGetCECVersion) {
    // GET_CEC_VERSION (0x9F)
    uint8_t testData[] = {0x40, 0x9F};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeCECVersion) {
    // CEC_VERSION (0x9E) with version number
    uint8_t testData[] = {0x04, 0x9E, 0x05};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeSetMenuLanguage) {
    // SET_MENU_LANGUAGE (0x32) with language code
    uint8_t testData[] = {0x0F, 0x32, 0x65, 0x6E, 0x67}; // "eng"
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeGiveOSDName) {
    // GIVE_OSD_NAME (0x46)
    uint8_t testData[] = {0x40, 0x46};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeGivePhysicalAddress) {
    // GIVE_PHYSICAL_ADDRESS (0x83)
    uint8_t testData[] = {0x40, 0x83};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeReportPhysicalAddress) {
    // REPORT_PHYSICAL_ADDRESS (0x84) with physical address and device type
    uint8_t testData[] = {0x4F, 0x84, 0x10, 0x00, 0x04};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeGiveDeviceVendorID) {
    // GIVE_DEVICE_VENDOR_ID (0x8C)
    uint8_t testData[] = {0x40, 0x8C};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeDeviceVendorID) {
    // DEVICE_VENDOR_ID (0x87) with vendor ID
    uint8_t testData[] = {0x04, 0x87, 0x00, 0x00, 0x80};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeRoutingChange) {
    // ROUTING_CHANGE (0x80) with old and new physical addresses
    uint8_t testData[] = {0x0F, 0x80, 0x00, 0x00, 0x10, 0x00};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeRoutingInformation) {
    // ROUTING_INFORMATION (0x81) with physical address
    uint8_t testData[] = {0x0F, 0x81, 0x10, 0x00};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeSetStreamPath) {
    // SET_STREAM_PATH (0x86) with physical address
    uint8_t testData[] = {0x0F, 0x86, 0x10, 0x00};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeGetMenuLanguage) {
    // GET_MENU_LANGUAGE (0x91)
    uint8_t testData[] = {0x40, 0x91};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeSetOSDString) {
    // SET_OSD_STRING (0x64) with display control and string
    uint8_t testData[] = {0x40, 0x64, 0x00, 0x54, 0x65, 0x73, 0x74}; // "Test"
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeSetOSDName) {
    // SET_OSD_NAME (0x47) with name
    uint8_t testData[] = {0x04, 0x47, 0x54, 0x56}; // "TV"
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeUserControlPressed) {
    // USER_CONTROL_PRESSED (0x44) with UI command
    uint8_t testData[] = {0x40, 0x44, 0x00}; // Select button
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeUserControlReleased) {
    // USER_CONTROL_RELEASED (0x45)
    uint8_t testData[] = {0x40, 0x45};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeGiveDevicePowerStatus) {
    // GIVE_DEVICE_POWER_STATUS (0x8F)
    uint8_t testData[] = {0x40, 0x8F};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeReportPowerStatus) {
    // REPORT_POWER_STATUS (0x90) with power status
    uint8_t testData[] = {0x04, 0x90, 0x00}; // Power on
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeFeatureAbort) {
    // FEATURE_ABORT (0x00) with feature opcode and abort reason
    uint8_t testData[] = {0x04, 0x00, 0x82, 0x04};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeAbort) {
    // ABORT (0xFF)
    uint8_t testData[] = {0x40, 0xFF};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeInitiateArc) {
    // INITIATE_ARC (0xC0)
    uint8_t testData[] = {0x50, 0xC0};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeTerminateArc) {
    // TERMINATE_ARC (0xC5)
    uint8_t testData[] = {0x50, 0xC5};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeRequestShortAudioDescriptor) {
    // REQUEST_SHORT_AUDIO_DESCRIPTOR (0xA4) with audio format codes
    uint8_t testData[] = {0x50, 0xA4, 0x01};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeReportShortAudioDescriptor) {
    // REPORT_SHORT_AUDIO_DESCRIPTOR (0xA3) with audio descriptors
    uint8_t testData[] = {0x05, 0xA3, 0x09, 0x07, 0x15};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeSystemAudioModeRequest) {
    // SYSTEM_AUDIO_MODE_REQUEST (0x70) with physical address
    uint8_t testData[] = {0x05, 0x70, 0x10, 0x00};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeSetSystemAudioMode) {
    // SET_SYSTEM_AUDIO_MODE (0x72) with status
    uint8_t testData[] = {0x0F, 0x72, 0x01};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeReportAudioStatus) {
    // REPORT_AUDIO_STATUS (0x7A) with audio status
    uint8_t testData[] = {0x05, 0x7A, 0x50};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeGiveFeatures) {
    // GIVE_FEATURES (0xA5)
    uint8_t testData[] = {0x40, 0xA5};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeReportFeatures) {
    // REPORT_FEATURES (0xA6) with CEC version and feature data
    uint8_t testData[] = {0x04, 0xA6, 0x05, 0x80, 0x00, 0x00};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeRequestCurrentLatency) {
    // REQUEST_CURRENT_LATENCY (0xA7) with physical address
    uint8_t testData[] = {0x50, 0xA7, 0x10, 0x00};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeReportCurrentLatency) {
    // REPORT_CURRENT_LATENCY (0xA8) with physical address and latency data
    uint8_t testData[] = {0x05, 0xA8, 0x10, 0x00, 0x01, 0x00, 0x20};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeVendorCommand) {
    // VENDOR_COMMAND (0x89) - should not throw
    uint8_t testData[] = {0x40, 0x89, 0x01, 0x02, 0x03};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeVendorCommandWithID) {
    // VENDOR_COMMAND_WITH_ID (0xA0) - should not throw
    uint8_t testData[] = {0x40, 0xA0, 0x00, 0x00, 0x80, 0x01};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeVendorRemoteButtonDown) {
    // VENDOR_REMOTE_BUTTON_DOWN (0x8A) - should not throw
    uint8_t testData[] = {0x40, 0x8A, 0x01};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeVendorRemoteButtonUp) {
    // VENDOR_REMOTE_BUTTON_UP (0x8B) - should not throw
    uint8_t testData[] = {0x40, 0x8B};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeUnknownOpCode) {
    // Unknown opcode (0x99) - should not throw, just log
    uint8_t testData[] = {0x40, 0x99, 0x01};
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

TEST_F(MessageDecoderTest, DecodeInvalidFrame) {
    // Frame with invalid parameters should catch exception internally
    uint8_t testData[] = {0x40, 0x82}; // ACTIVE_SOURCE without physical address
    CECFrame frame(testData, sizeof(testData));
    EXPECT_NO_THROW({
        decoder.decode(frame);
    });
}

// ============================================================================
// Dispatch-verification tests using a TrackingProcessor that records which
// process() overload the decoder invoked and captures key operand values.
// ============================================================================

class TrackingProcessor : public MessageProcessor {
public:
    std::string lastProcessed;
    PhysicalAddress lastPhysAddress{0,0,0,0};
    int           lastPowerStatus{-1};
    int           lastVersionValue{-1};
    int           lastAbortReason{-1};
    std::string   lastOSDName;

    void process(const ImageViewOn &, const Header &) override {
        lastProcessed = "ImageViewOn";
    }
    void process(const TextViewOn &, const Header &) override {
        lastProcessed = "TextViewOn";
    }
    void process(const ActiveSource &msg, const Header &) override {
        lastProcessed = "ActiveSource";
        lastPhysAddress = msg.physicalAddress;
    }
    void process(const InActiveSource &msg, const Header &) override {
        lastProcessed = "InActiveSource";
        lastPhysAddress = msg.physicalAddress;
    }
    void process(const Standby &, const Header &) override {
        lastProcessed = "Standby";
    }
    void process(const GetCECVersion &, const Header &) override {
        lastProcessed = "GetCECVersion";
    }
    void process(const CECVersion &msg, const Header &) override {
        lastProcessed = "CECVersion";
        // Version has no toInt(); extract the raw byte by serializing into a frame.
        CECFrame f;
        msg.version.serialize(f);
        lastVersionValue = static_cast<int>(f.at(0));
    }
    void process(const ReportPowerStatus &msg, const Header &) override {
        lastProcessed = "ReportPowerStatus";
        lastPowerStatus = msg.status.toInt();
    }
    void process(const FeatureAbort &msg, const Header &) override {
        lastProcessed = "FeatureAbort";
        lastAbortReason = msg.reason.toInt();
    }
    void process(const Abort &, const Header &) override {
        lastProcessed = "Abort";
    }
    void process(const Polling &, const Header &) override {
        lastProcessed = "Polling";
    }
    void process(const GivePhysicalAddress &, const Header &) override {
        lastProcessed = "GivePhysicalAddress";
    }
    void process(const ReportPhysicalAddress &msg, const Header &) override {
        lastProcessed = "ReportPhysicalAddress";
        lastPhysAddress = msg.physicalAddress;
    }
    void process(const SetOSDName &msg, const Header &) override {
        lastProcessed = "SetOSDName";
        lastOSDName = msg.osdName.toString();
    }
    void process(const GiveDevicePowerStatus &, const Header &) override {
        lastProcessed = "GiveDevicePowerStatus";
    }
    void process(const RequestActiveSource &, const Header &) override {
        lastProcessed = "RequestActiveSource";
    }
};

class MessageDecoderTrackingTest : public ::testing::Test {
protected:
    TrackingProcessor tracking;
    MessageDecoder decoder{tracking};
};

TEST_F(MessageDecoderTrackingTest, ImageViewOnDispatch) {
    // from=TV(0), to=PLAYBACK_DEVICE_1(4), opcode=IMAGE_VIEW_ON(0x04)
    uint8_t data[] = {0x04, 0x04};
    CECFrame frame(data, sizeof(data));
    decoder.decode(frame);
    EXPECT_EQ(tracking.lastProcessed, "ImageViewOn");
}

TEST_F(MessageDecoderTrackingTest, StandbyDispatch) {
    // from=PLAYBACK_DEVICE_1(4), to=TV(0), opcode=STANDBY(0x36)
    uint8_t data[] = {0x40, 0x36};
    CECFrame frame(data, sizeof(data));
    decoder.decode(frame);
    EXPECT_EQ(tracking.lastProcessed, "Standby");
}

TEST_F(MessageDecoderTrackingTest, ActiveSourceDispatchAndPhysicalAddress) {
    // from=PLAYBACK_DEVICE_1(4), to=BROADCAST(0xF), opcode=ACTIVE_SOURCE(0x82)
    // Physical address 1.0.0.0: byte1=0x10, byte2=0x00
    uint8_t data[] = {0x4F, 0x82, 0x10, 0x00};
    CECFrame frame(data, sizeof(data));
    decoder.decode(frame);
    EXPECT_EQ(tracking.lastProcessed, "ActiveSource");
    EXPECT_STREQ(tracking.lastPhysAddress.toString().c_str(), "1.0.0.0");
}

TEST_F(MessageDecoderTrackingTest, ReportPowerStatusDispatchAndValue) {
    // opcode=REPORT_POWER_STATUS(0x90), power_status=ON(0x00)
    uint8_t data[] = {0x04, 0x90, 0x00};
    CECFrame frame(data, sizeof(data));
    decoder.decode(frame);
    EXPECT_EQ(tracking.lastProcessed, "ReportPowerStatus");
    EXPECT_EQ(tracking.lastPowerStatus, (int)PowerStatus::ON); // 0
}

TEST_F(MessageDecoderTrackingTest, CECVersionDispatchAndValue) {
    // opcode=CEC_VERSION(0x9E), version=V_1_4(0x05)
    uint8_t data[] = {0x04, 0x9E, 0x05};
    CECFrame frame(data, sizeof(data));
    decoder.decode(frame);
    EXPECT_EQ(tracking.lastProcessed, "CECVersion");
    EXPECT_EQ(tracking.lastVersionValue, (int)Version::V_1_4); // 5
}

TEST_F(MessageDecoderTrackingTest, FeatureAbortDispatchAndReason) {
    // opcode=FEATURE_ABORT(0x00), feature=ACTIVE_SOURCE(0x82), reason=REFUSED(4)
    uint8_t data[] = {0x04, 0x00, 0x82, 0x04};
    CECFrame frame(data, sizeof(data));
    decoder.decode(frame);
    EXPECT_EQ(tracking.lastProcessed, "FeatureAbort");
    EXPECT_EQ(tracking.lastAbortReason, (int)AbortReason::REFUSED); // 4
}

TEST_F(MessageDecoderTrackingTest, PollingMessageDispatch) {
    // Single-byte frame: from=PLAYBACK_DEVICE_1(4), to=PLAYBACK_DEVICE_1(4) -> 0x44
    uint8_t data[] = {0x44};
    CECFrame frame(data, sizeof(data));
    decoder.decode(frame);
    EXPECT_EQ(tracking.lastProcessed, "Polling");
}

TEST_F(MessageDecoderTrackingTest, ReportPhysicalAddressDispatchAndValue) {
    // opcode=REPORT_PHYSICAL_ADDRESS(0x84), addr=1.0.0.0, type=PLAYBACK_DEVICE(4)
    uint8_t data[] = {0x4F, 0x84, 0x10, 0x00, 0x04};
    CECFrame frame(data, sizeof(data));
    decoder.decode(frame);
    EXPECT_EQ(tracking.lastProcessed, "ReportPhysicalAddress");
    EXPECT_STREQ(tracking.lastPhysAddress.toString().c_str(), "1.0.0.0");
}

TEST_F(MessageDecoderTrackingTest, GivePhysicalAddressDispatch) {
    // opcode=GIVE_PHYSICAL_ADDRESS(0x83)
    uint8_t data[] = {0x40, 0x83};
    CECFrame frame(data, sizeof(data));
    decoder.decode(frame);
    EXPECT_EQ(tracking.lastProcessed, "GivePhysicalAddress");
}

TEST_F(MessageDecoderTrackingTest, RequestActiveSourceDispatch) {
    // opcode=REQUEST_ACTIVE_SOURCE(0x85), broadcast
    uint8_t data[] = {0x4F, 0x85};
    CECFrame frame(data, sizeof(data));
    decoder.decode(frame);
    EXPECT_EQ(tracking.lastProcessed, "RequestActiveSource");
}
