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
#include "ccec/OpCode.hpp"
#include "ccec/CECFrame.hpp"



class OpCodeTest : public ::testing::Test {};

TEST_F(OpCodeTest, OpCodeConstants) {
    EXPECT_EQ(IMAGE_VIEW_ON, 0x04);
    EXPECT_EQ(TEXT_VIEW_ON, 0x0D);
    EXPECT_EQ(STANDBY, 0x36);
    EXPECT_EQ(ACTIVE_SOURCE, 0x82);
    EXPECT_EQ(INACTIVE_SOURCE, 0x9D);
}

TEST_F(OpCodeTest, OpCodeToString) {
    OpCode opcode(IMAGE_VIEW_ON);
    EXPECT_NO_THROW({
        std::string name = opcode.toString();
        EXPECT_FALSE(name.empty());
    });
}

// Test OpCode construction from value
TEST_F(OpCodeTest, OpCodeConstructor) {
    OpCode opcode(ACTIVE_SOURCE);
    EXPECT_EQ(opcode.opCode(), ACTIVE_SOURCE);
}

// Test OpCode construction from CECFrame
TEST_F(OpCodeTest, OpCodeFromFrame) {
    uint8_t data[] = {0x40, 0x04}; // Header + IMAGE_VIEW_ON
    CECFrame frame(data, sizeof(data));
    OpCode opcode(frame, 1); // OpCode at position 1
    EXPECT_EQ(opcode.opCode(), IMAGE_VIEW_ON);
}

// Test serialize method
TEST_F(OpCodeTest, OpCodeSerialize) {
    OpCode opcode(TEXT_VIEW_ON);
    CECFrame frame;
    opcode.serialize(frame);
    EXPECT_EQ(frame.length(), 1u);
    EXPECT_EQ(frame.at(0), TEXT_VIEW_ON);
}

// Test serialize with POLLING (should not append)
TEST_F(OpCodeTest, OpCodeSerializePolling) {
    OpCode opcode(POLLING);
    CECFrame frame;
    opcode.serialize(frame);
    EXPECT_EQ(frame.length(), 0u); // POLLING should not be added to frame
}

// Test print method
TEST_F(OpCodeTest, OpCodePrint) {
    OpCode opcode(STANDBY);
    EXPECT_NO_THROW({
        opcode.print();
    });
}

// Test all opcode names for coverage
TEST_F(OpCodeTest, GetOpNameActiveSource) {
    EXPECT_STREQ(GetOpName(ACTIVE_SOURCE), "Active Source");
}

TEST_F(OpCodeTest, GetOpNameImageViewOn) {
    EXPECT_STREQ(GetOpName(IMAGE_VIEW_ON), "Image View On");
}

TEST_F(OpCodeTest, GetOpNameTextViewOn) {
    EXPECT_STREQ(GetOpName(TEXT_VIEW_ON), "Text View On");
}

TEST_F(OpCodeTest, GetOpNameInactiveSource) {
    EXPECT_STREQ(GetOpName(INACTIVE_SOURCE), "InActive Source");
}

TEST_F(OpCodeTest, GetOpNameRequestActiveSource) {
    EXPECT_STREQ(GetOpName(REQUEST_ACTIVE_SOURCE), "Request Active Source");
}

TEST_F(OpCodeTest, GetOpNameRoutingChange) {
    EXPECT_STREQ(GetOpName(ROUTING_CHANGE), "Routing Change");
}

TEST_F(OpCodeTest, GetOpNameRoutingInformation) {
    EXPECT_STREQ(GetOpName(ROUTING_INFORMATION), "Routing Information");
}

TEST_F(OpCodeTest, GetOpNameSetStreamPath) {
    EXPECT_STREQ(GetOpName(SET_STREAM_PATH), "Set Stream Path");
}

TEST_F(OpCodeTest, GetOpNameStandby) {
    EXPECT_STREQ(GetOpName(STANDBY), "Stand by");
}

TEST_F(OpCodeTest, GetOpNameRecordOff) {
    EXPECT_STREQ(GetOpName(RECORD_OFF), "Record Off");
}

TEST_F(OpCodeTest, GetOpNameRecordOn) {
    EXPECT_STREQ(GetOpName(RECORD_ON), " Record On");
}

TEST_F(OpCodeTest, GetOpNameRecordStatus) {
    EXPECT_STREQ(GetOpName(RECORD_STATUS), "Record Status");
}

TEST_F(OpCodeTest, GetOpNameRecordTVScreen) {
    EXPECT_STREQ(GetOpName(RECORD_TV_SCREEN), "Record TV Screen");
}

TEST_F(OpCodeTest, GetOpNameClearAnalogueTimer) {
    EXPECT_STREQ(GetOpName(CLEAR_ANALOGUE_TIMER), "Clear Analogue Timer");
}

TEST_F(OpCodeTest, GetOpNameClearDigitalTimer) {
    EXPECT_STREQ(GetOpName(CLEAR_DIGITAL_TIMER), "Clear Digital Timer");
}

TEST_F(OpCodeTest, GetOpNameClearExternalTimer) {
    EXPECT_STREQ(GetOpName(CLEAR_EXTERNAL_TIMER), "Clear External Timer");
}

TEST_F(OpCodeTest, GetOpNameSetAnalogTimer) {
    EXPECT_STREQ(GetOpName(SET_ANALOG_TIMER), "Set Analog Timer");
}

TEST_F(OpCodeTest, GetOpNameSetDigitalTimer) {
    EXPECT_STREQ(GetOpName(SET_DIGITAL_TIMER), " Set Digital Timer");
}

TEST_F(OpCodeTest, GetOpNameSetExternalTimer) {
    EXPECT_STREQ(GetOpName(SET_EXTERNAL_TIMER), "Set External Timer");
}

TEST_F(OpCodeTest, GetOpNameSetTimerProgramTitle) {
    EXPECT_STREQ(GetOpName(SET_TIMER_PROGRAM_TITLE), "Set Timer Program Title");
}

TEST_F(OpCodeTest, GetOpNameTimerClearedStatus) {
    EXPECT_STREQ(GetOpName(TIMER_CLEARED_STATUS), "Timer Cleared Status");
}

TEST_F(OpCodeTest, GetOpNameTimerStatus) {
    EXPECT_STREQ(GetOpName(TIMER_STATUS), "Timer Status");
}

TEST_F(OpCodeTest, GetOpNameCECVersion) {
    EXPECT_STREQ(GetOpName(CEC_VERSION), "CEC Version");
}

TEST_F(OpCodeTest, GetOpNameGivePhysicalAddress) {
    EXPECT_STREQ(GetOpName(GIVE_PHYSICAL_ADDRESS), "Give Physical Address");
}

TEST_F(OpCodeTest, GetOpNameGetMenuLanguage) {
    EXPECT_STREQ(GetOpName(GET_MENU_LANGUAGE), "Get Menu Language");
}

TEST_F(OpCodeTest, GetOpNamePolling) {
    EXPECT_STREQ(GetOpName(POLLING), "Polling ");
}

TEST_F(OpCodeTest, GetOpNameReportPhysicalAddress) {
    EXPECT_STREQ(GetOpName(REPORT_PHYSICAL_ADDRESS), "Report Physical Address");
}

TEST_F(OpCodeTest, GetOpNameSetMenuLanguage) {
    EXPECT_STREQ(GetOpName(SET_MENU_LANGUAGE), "Set Menu Language");
}

TEST_F(OpCodeTest, GetOpNameDeckControl) {
    EXPECT_STREQ(GetOpName(DECK_CONTROL), "Deck control");
}

TEST_F(OpCodeTest, GetOpNameDeckStatus) {
    EXPECT_STREQ(GetOpName(DECK_STATUS), "deck Status");
}

TEST_F(OpCodeTest, GetOpNamePlay) {
    EXPECT_STREQ(GetOpName(PLAY), "Play");
}

TEST_F(OpCodeTest, GetOpNameGiveTunerDeviceStatus) {
    EXPECT_STREQ(GetOpName(GIVE_TUNER_DEVICE_STATUS), "Give Tuner Device Status");
}

TEST_F(OpCodeTest, GetOpNameSelectAnalogueService) {
    EXPECT_STREQ(GetOpName(SELECT_ANALOGUE_SERVICE), "Select Analogue service");
}

TEST_F(OpCodeTest, GetOpNameTunerDeviceStatus) {
    EXPECT_STREQ(GetOpName(TUNER_DEVICE_STATUS), "Tuner Device Status");
}

TEST_F(OpCodeTest, GetOpNameTunerStepDecrement) {
    EXPECT_STREQ(GetOpName(TUNER_STEP_DECREMENT), "Tuner Step Decrement");
}

TEST_F(OpCodeTest, GetOpNameTunerStepIncrement) {
    EXPECT_STREQ(GetOpName(TUNER_STEP_INCREMENT), "Tuner Step Increment");
}

TEST_F(OpCodeTest, GetOpNameDeviceVendorID) {
    EXPECT_STREQ(GetOpName(DEVICE_VENDOR_ID), "Device Vendor Id");
}

TEST_F(OpCodeTest, GetOpNameGetCECVersion) {
    EXPECT_STREQ(GetOpName(GET_CEC_VERSION), "Get CEC Version");
}

TEST_F(OpCodeTest, GetOpNameGiveDeviceVendorID) {
    EXPECT_STREQ(GetOpName(GIVE_DEVICE_VENDOR_ID), "Give Ddevice Vendor ID");
}

TEST_F(OpCodeTest, GetOpNameVendorCommand) {
    EXPECT_STREQ(GetOpName(VENDOR_COMMAND), "Vendor Command");
}

TEST_F(OpCodeTest, GetOpNameVendorCommandWithID) {
    EXPECT_STREQ(GetOpName(VENDOR_COMMAND_WITH_ID), "Vendor command With ID");
}

TEST_F(OpCodeTest, GetOpNameVendorRemoteButtonDown) {
    EXPECT_STREQ(GetOpName(VENDOR_REMOTE_BUTTON_DOWN), "Vendor Remote Button Down");
}

TEST_F(OpCodeTest, GetOpNameVendorRemoteButtonUp) {
    EXPECT_STREQ(GetOpName(VENDOR_REMOTE_BUTTON_UP), "Vendor Remote Button Up");
}

TEST_F(OpCodeTest, GetOpNameSetOSDString) {
    EXPECT_STREQ(GetOpName(SET_OSD_STRING), "Set OSD String");
}

TEST_F(OpCodeTest, GetOpNameGiveOSDName) {
    EXPECT_STREQ(GetOpName(GIVE_OSD_NAME), "Give OSD Name");
}

TEST_F(OpCodeTest, GetOpNameSetOSDName) {
    EXPECT_STREQ(GetOpName(SET_OSD_NAME), "Set OSD Name");
}

TEST_F(OpCodeTest, GetOpNameMenuRequest) {
    EXPECT_STREQ(GetOpName(MENU_REQUEST), "Menu Request");
}

TEST_F(OpCodeTest, GetOpNameMenuStatus) {
    EXPECT_STREQ(GetOpName(MENU_STATUS), "Menu Status");
}

TEST_F(OpCodeTest, GetOpNameUserControlPressed) {
    EXPECT_STREQ(GetOpName(USER_CONTROL_PRESSED), "User control Pressed");
}

TEST_F(OpCodeTest, GetOpNameUserControlReleased) {
    EXPECT_STREQ(GetOpName(USER_CONTROL_RELEASED), "User Control released");
}

TEST_F(OpCodeTest, GetOpNameGiveDevicePowerStatus) {
    EXPECT_STREQ(GetOpName(GIVE_DEVICE_POWER_STATUS), "Give Device Power Status");
}

TEST_F(OpCodeTest, GetOpNameReportPowerStatus) {
    EXPECT_STREQ(GetOpName(REPORT_POWER_STATUS), "Report power Status");
}

TEST_F(OpCodeTest, GetOpNameFeatureAbort) {
    EXPECT_STREQ(GetOpName(FEATURE_ABORT), "Feature Abort");
}

TEST_F(OpCodeTest, GetOpNameAbort) {
    EXPECT_STREQ(GetOpName(ABORT), "Abort");
}

TEST_F(OpCodeTest, GetOpNameGiveAudioStatus) {
    EXPECT_STREQ(GetOpName(GIVE_AUDIO_STATUS), "Give Aduio Status");
}

TEST_F(OpCodeTest, GetOpNameGiveSystemAudioModeStatus) {
    EXPECT_STREQ(GetOpName(GIVE_SYSTEM_AUDIO_MODE_STATUS), "Give System Audio Mode Status");
}

TEST_F(OpCodeTest, GetOpNameReportAudioStatus) {
    EXPECT_STREQ(GetOpName(REPORT_AUDIO_STATUS), "Report Audio Status");
}

TEST_F(OpCodeTest, GetOpNameRequestShortAudioDescriptor) {
    EXPECT_STREQ(GetOpName(REQUEST_SHORT_AUDIO_DESCRIPTOR), "Request Short Audio Descriptor");
}

TEST_F(OpCodeTest, GetOpNameReportShortAudioDescriptor) {
    EXPECT_STREQ(GetOpName(REPORT_SHORT_AUDIO_DESCRIPTOR), "Report Short Audio Descriptor");
}

TEST_F(OpCodeTest, GetOpNameSetSystemAudioMode) {
    EXPECT_STREQ(GetOpName(SET_SYSTEM_AUDIO_MODE), "Set System Audio Mode");
}

TEST_F(OpCodeTest, GetOpNameSystemAudioModeRequest) {
    EXPECT_STREQ(GetOpName(SYSTEM_AUDIO_MODE_REQUEST), "System Audio mode request");
}

TEST_F(OpCodeTest, GetOpNameSetAudioRate) {
    EXPECT_STREQ(GetOpName(SET_AUDIO_RATE), "Set Audio rate");
}

TEST_F(OpCodeTest, GetOpNameInitiateARC) {
    EXPECT_STREQ(GetOpName(INITIATE_ARC), "Initiate ARC");
}

TEST_F(OpCodeTest, GetOpNameReportARCInitiated) {
    EXPECT_STREQ(GetOpName(REPORT_ARC_INITIATED), "Report ARC Initiated");
}

TEST_F(OpCodeTest, GetOpNameReportARCTerminated) {
    EXPECT_STREQ(GetOpName(REPORT_ARC_TERMINATED), "Report ARC Terminated");
}

TEST_F(OpCodeTest, GetOpNameRequestARCInitiation) {
    EXPECT_STREQ(GetOpName(REQUEST_ARC_INITIATION), "Report ARC Initiation");
}

TEST_F(OpCodeTest, GetOpNameRequestARCTermination) {
    EXPECT_STREQ(GetOpName(REQUEST_ARC_TERMINATION), "Request ARC Termination");
}

TEST_F(OpCodeTest, GetOpNameTerminateARC) {
    EXPECT_STREQ(GetOpName(TERMINATE_ARC), "Terminate ARC");
}

TEST_F(OpCodeTest, GetOpNameCDCMessage) {
    EXPECT_STREQ(GetOpName(CDC_MESSAGE), "CDC Message");
}

TEST_F(OpCodeTest, GetOpNameGiveFeatures) {
    EXPECT_STREQ(GetOpName(GIVE_FEATURES), "Give Features");
}

TEST_F(OpCodeTest, GetOpNameReportFeatures) {
    EXPECT_STREQ(GetOpName(REPORT_FEATURES), "Report Features");
}

TEST_F(OpCodeTest, GetOpNameRequestCurrentLatency) {
    EXPECT_STREQ(GetOpName(REQUEST_CURRENT_LATENCY), "Request Current Latency");
}

TEST_F(OpCodeTest, GetOpNameReportCurrentLatency) {
    EXPECT_STREQ(GetOpName(REPORT_CURRENT_LATENCY), "Report Current Latency");
}

TEST_F(OpCodeTest, GetOpNameUnrecognized) {
    // Use an opcode value that's not defined (0x99 is CLEAR_DIGITAL_TIMER)
    EXPECT_STREQ(GetOpName(0xAA), "Unrecognized Message");
}

// Test OpCode methods with various opcodes
TEST_F(OpCodeTest, OpCodeMethodsWithDifferentOpcodes) {
    // Test various opcodes
    std::vector<Op_t> opcodes = {
        ACTIVE_SOURCE,
        STANDBY,
        REPORT_POWER_STATUS,
        FEATURE_ABORT,
        GIVE_FEATURES,
        TERMINATE_ARC,
        USER_CONTROL_PRESSED
    };
    
    for (auto op : opcodes) {
        OpCode opcode(op);
        EXPECT_EQ(opcode.opCode(), op);
        EXPECT_NO_THROW({
            std::string name = opcode.toString();
            EXPECT_FALSE(name.empty());
            opcode.print();
        });
    }
}
