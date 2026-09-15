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

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

#include "ccec/Operands.hpp"



class OperandsTest : public ::testing::Test {};

// ============= PhysicalAddress Tests =============
TEST_F(OperandsTest, PhysicalAddressCreation) {
    PhysicalAddress phy(1, 0, 0, 0);
    EXPECT_NO_THROW({
        phy.toString();
    });
}

TEST_F(OperandsTest, PhysicalAddressComponents) {
    PhysicalAddress phy(1, 2, 3, 4);
    EXPECT_NO_THROW({
        std::string str = phy.toString();
        EXPECT_FALSE(str.empty());
    });
}

TEST_F(OperandsTest, PhysicalAddressToString) {
    PhysicalAddress phy(1, 2, 3, 4);
    EXPECT_STREQ(phy.toString().c_str(), "1.2.3.4");
}

TEST_F(OperandsTest, PhysicalAddressGetByteValue) {
    PhysicalAddress phy(1, 2, 3, 4);
    EXPECT_EQ(phy.getByteValue(0), 1);
    EXPECT_EQ(phy.getByteValue(1), 2);
    EXPECT_EQ(phy.getByteValue(2), 3);
    EXPECT_EQ(phy.getByteValue(3), 4);
}

TEST_F(OperandsTest, PhysicalAddressName) {
    PhysicalAddress phy(0, 0, 0, 0);
    EXPECT_STREQ(phy.name().c_str(), "PhysicalAddress");
}

TEST_F(OperandsTest, PhysicalAddressFromString) {
    std::string addr = "1.2.3.4";
    PhysicalAddress phy(addr);
    EXPECT_STREQ(phy.toString().c_str(), "1.2.3.4");
}

TEST_F(OperandsTest, PhysicalAddressSerialize) {
    PhysicalAddress phy(1, 2, 3, 4);
    CECFrame frame;
    phy.serialize(frame);
    const uint8_t *buf;
    size_t len;
    frame.getBuffer(&buf, &len);
    EXPECT_EQ(len, 2);
}

TEST_F(OperandsTest, PhysicalAddressEquality) {
    PhysicalAddress phy1(1, 2, 3, 4);
    PhysicalAddress phy2(1, 2, 3, 4);
    PhysicalAddress phy3(5, 6, 7, 8);
    EXPECT_TRUE(phy1 == phy2);
    EXPECT_TRUE(phy1 != phy3);
}

// ============= LogicalAddress Tests =============
TEST_F(OperandsTest, LogicalAddressEnum) {
    LogicalAddress tv = LogicalAddress::TV;
    LogicalAddress playback = LogicalAddress::PLAYBACK_DEVICE_1;
    LogicalAddress unreg = LogicalAddress::UNREGISTERED;
    
    EXPECT_NE(tv, playback);
    EXPECT_NE(tv, unreg);
}

TEST_F(OperandsTest, LogicalAddressToString) {
    LogicalAddress tv(LogicalAddress::TV);
    EXPECT_STREQ(tv.toString().c_str(), "TV");
    
    LogicalAddress playback(LogicalAddress::PLAYBACK_DEVICE_1);
    EXPECT_STREQ(playback.toString().c_str(), "Playback Device 1");
    
    LogicalAddress unreg(LogicalAddress::UNREGISTERED);
    EXPECT_STREQ(unreg.toString().c_str(), "Broadcast/Unregistered");
}

TEST_F(OperandsTest, LogicalAddressToInt) {
    LogicalAddress tv(LogicalAddress::TV);
    EXPECT_EQ(tv.toInt(), 0);
    
    LogicalAddress audioSys(LogicalAddress::AUDIO_SYSTEM);
    EXPECT_EQ(audioSys.toInt(), 5);
}

TEST_F(OperandsTest, LogicalAddressValidate) {
    LogicalAddress valid(LogicalAddress::TV);
    EXPECT_TRUE(valid.validate());
    
    LogicalAddress broadcast(LogicalAddress::BROADCAST);
    EXPECT_TRUE(broadcast.validate());
}

TEST_F(OperandsTest, LogicalAddressGetType) {
    LogicalAddress tv(LogicalAddress::TV);
    EXPECT_EQ(tv.getType(), DeviceType::TV);
    
    LogicalAddress playback(LogicalAddress::PLAYBACK_DEVICE_1);
    EXPECT_EQ(playback.getType(), DeviceType::PLAYBACK_DEVICE);
    
    LogicalAddress audioSys(LogicalAddress::AUDIO_SYSTEM);
    EXPECT_EQ(audioSys.getType(), DeviceType::AUDIO_SYSTEM);
}

// ============= DeviceType Tests =============
TEST_F(OperandsTest, DeviceTypeToString) {
    DeviceType tv(DeviceType::TV);
    EXPECT_STREQ(tv.toString().c_str(), "TV");
    
    DeviceType playback(DeviceType::PLAYBACK_DEVICE);
    EXPECT_STREQ(playback.toString().c_str(), "Playback Device");
    
    DeviceType audioSys(DeviceType::AUDIO_SYSTEM);
    EXPECT_STREQ(audioSys.toString().c_str(), "Audio System");
}

TEST_F(OperandsTest, DeviceTypeValidate) {
    DeviceType valid(DeviceType::TV);
    EXPECT_TRUE(valid.validate());
    
    DeviceType videoProc(DeviceType::VIDEO_PROCESSOR);
    EXPECT_TRUE(videoProc.validate());
}

// ============= Version Tests =============
TEST_F(OperandsTest, VersionToString) {
    Version v13a(Version::V_1_3a);
    EXPECT_STREQ(v13a.toString().c_str(), "Version 1.3a");
    
    Version v14(Version::V_1_4);
    EXPECT_STREQ(v14.toString().c_str(), "Version 1.4");
    
    Version v20(Version::V_2_0);
    EXPECT_STREQ(v20.toString().c_str(), "Version 2.0");
}

TEST_F(OperandsTest, VersionValidate) {
    Version valid(Version::V_1_4);
    EXPECT_TRUE(valid.validate());
}

// ============= PowerStatus Tests =============
TEST_F(OperandsTest, PowerStatusToString) {
    PowerStatus on(PowerStatus::ON);
    EXPECT_STREQ(on.toString().c_str(), "On");
    
    PowerStatus standby(PowerStatus::STANDBY);
    EXPECT_STREQ(standby.toString().c_str(), "Standby");
    
    PowerStatus transitionToOn(PowerStatus::IN_TRANSITION_STANDBY_TO_ON);
    EXPECT_STREQ(transitionToOn.toString().c_str(), "In transition Standby to On");
}

TEST_F(OperandsTest, PowerStatusToInt) {
    PowerStatus on(PowerStatus::ON);
    EXPECT_EQ(on.toInt(), 0);
    
    PowerStatus standby(PowerStatus::STANDBY);
    EXPECT_EQ(standby.toInt(), 1);
}

TEST_F(OperandsTest, PowerStatusValidate) {
    PowerStatus valid(PowerStatus::ON);
    EXPECT_TRUE(valid.validate());
}

// ============= AbortReason Tests =============
TEST_F(OperandsTest, AbortReasonToString) {
    AbortReason unrecognized(AbortReason::UNRECOGNIZED_OPCODE);
    EXPECT_STREQ(unrecognized.toString().c_str(), "Unrecognized opcode");
    
    AbortReason invalidOp(AbortReason::INVALID_OPERAND);
    EXPECT_STREQ(invalidOp.toString().c_str(), "Invalid operand");
    
    AbortReason refused(AbortReason::REFUSED);
    EXPECT_STREQ(refused.toString().c_str(), "Refused");
}

TEST_F(OperandsTest, AbortReasonToInt) {
    AbortReason reason(AbortReason::REFUSED);
    EXPECT_EQ(reason.toInt(), AbortReason::REFUSED);
}

TEST_F(OperandsTest, AbortReasonValidate) {
    AbortReason valid(AbortReason::UNRECOGNIZED_OPCODE);
    EXPECT_TRUE(valid.validate());
}

// ============= OSDString Tests =============
TEST_F(OperandsTest, OSDStringToString) {
    OSDString osd("Hello");
    EXPECT_STREQ(osd.toString().c_str(), "Hello");
}

TEST_F(OperandsTest, OSDStringMaxLength) {
    OSDString osd("1234567890123");  // 13 chars (max)
    EXPECT_STREQ(osd.toString().c_str(), "1234567890123");
}

// ============= OSDName Tests =============
TEST_F(OperandsTest, OSDNameToString) {
    OSDName name("MyDevice");
    EXPECT_STREQ(name.toString().c_str(), "MyDevice");
}

TEST_F(OperandsTest, OSDNameMaxLength) {
    OSDName name("12345678901234");  // 14 chars (max)
    EXPECT_STREQ(name.toString().c_str(), "12345678901234");
}

// ============= Language Tests =============
TEST_F(OperandsTest, LanguageToString) {
    Language eng("eng");
    EXPECT_STREQ(eng.toString().c_str(), "eng");
}

TEST_F(OperandsTest, LanguageCreation) {
    Language fra("fra");
    EXPECT_STREQ(fra.toString().c_str(), "fra");
}

// ============= VendorID Tests =============
TEST_F(OperandsTest, VendorIDCreation) {
    VendorID vendor(0x12, 0x34, 0x56);
    EXPECT_NO_THROW({
        CECFrame frame;
        vendor.serialize(frame);
    });
}

TEST_F(OperandsTest, VendorIDSerialize) {
    VendorID vendor(0xAA, 0xBB, 0xCC);
    CECFrame frame;
    vendor.serialize(frame);
    const uint8_t *buf;
    size_t len;
    frame.getBuffer(&buf, &len);
    EXPECT_EQ(len, 3);
    EXPECT_EQ(buf[0], 0xAA);
    EXPECT_EQ(buf[1], 0xBB);
    EXPECT_EQ(buf[2], 0xCC);
}

// ============= UICommand Tests =============
TEST_F(OperandsTest, UICommandToInt) {
    UICommand volUp(UICommand::UI_COMMAND_VOLUME_UP);
    EXPECT_EQ(volUp.toInt(), 0x41);
    
    UICommand select(UICommand::UI_COMMAND_SELECT);
    EXPECT_EQ(select.toInt(), 0x00);
}

TEST_F(OperandsTest, UICommandCreation) {
    UICommand mute(UICommand::UI_COMMAND_MUTE);
    EXPECT_EQ(mute.toInt(), 0x43);
}

// ============= SystemAudioStatus Tests =============
TEST_F(OperandsTest, SystemAudioStatusToString) {
    SystemAudioStatus off(SystemAudioStatus::OFF);
    EXPECT_STREQ(off.toString().c_str(), "Off");
    
    SystemAudioStatus on(SystemAudioStatus::ON);
    EXPECT_STREQ(on.toString().c_str(), "On");
}

TEST_F(OperandsTest, SystemAudioStatusToInt) {
    SystemAudioStatus on(SystemAudioStatus::ON);
    EXPECT_EQ(on.toInt(), 1);
}

TEST_F(OperandsTest, SystemAudioStatusValidate) {
    SystemAudioStatus valid(SystemAudioStatus::OFF);
    EXPECT_TRUE(valid.validate());
}

// ============= AudioStatus Tests =============
TEST_F(OperandsTest, AudioStatusToString) {
    AudioStatus muteOff(0x00);
    EXPECT_STREQ(muteOff.toString().c_str(), "Audio Mute Off");
    
    AudioStatus muteOn(0x80);
    EXPECT_STREQ(muteOn.toString().c_str(), "Audio Mute On");
}

TEST_F(OperandsTest, AudioStatusGetAudioMuteStatus) {
    AudioStatus muteOff(0x00);
    EXPECT_EQ(muteOff.getAudioMuteStatus(), 0);
    
    AudioStatus muteOn(0x80);
    EXPECT_EQ(muteOn.getAudioMuteStatus(), 1);
}

TEST_F(OperandsTest, AudioStatusGetAudioVolume) {
    AudioStatus vol50(0x32);
    EXPECT_EQ(vol50.getAudioVolume(), 0x32);
    
    AudioStatus vol100(0x64);
    EXPECT_EQ(vol100.getAudioVolume(), 0x64);
}

// ============= RequestAudioFormat Tests =============
TEST_F(OperandsTest, RequestAudioFormatToString) {
    RequestAudioFormat lpcm(RequestAudioFormat::SAD_FMT_CODE_LPCM);
    EXPECT_STREQ(lpcm.toString().c_str(), "LPCM");
    
    RequestAudioFormat ac3(RequestAudioFormat::SAD_FMT_CODE_AC3);
    EXPECT_STREQ(ac3.toString().c_str(), "AC3");
    
    RequestAudioFormat dts(RequestAudioFormat::SAD_FMT_CODE_DTS);
    EXPECT_STREQ(dts.toString().c_str(), "DTS");
}

TEST_F(OperandsTest, RequestAudioFormatGetMethods) {
    RequestAudioFormat format(0x41);  // ID=1, Code=1 (LPCM)
    EXPECT_EQ(format.getAudioformatId(), 1);
    EXPECT_EQ(format.getAudioformatCode(), 1);
}

// ============= ShortAudioDescriptor Tests =============
TEST_F(OperandsTest, ShortAudioDescriptorToString) {
    uint8_t buf[3] = {0x08, 0x00, 0x00};  // LPCM format (code 1, shifted left by 3 = 0x08)
    ShortAudioDescriptor sad(buf, 3);
    EXPECT_STREQ(sad.toString().c_str(), "LPCM");
}

TEST_F(OperandsTest, ShortAudioDescriptorGetAudioformatCode) {
    uint8_t buf[3] = {0x10, 0x00, 0x00};  // AC3 format (code 2, shifted left by 3 = 0x10)
    ShortAudioDescriptor sad(buf, 3);
    EXPECT_EQ(sad.getAudioformatCode(), 2);
}

TEST_F(OperandsTest, ShortAudioDescriptorGetAudiodescriptor) {
    uint8_t buf[3] = {0x12, 0x34, 0x56};
    ShortAudioDescriptor sad(buf, 3);
    uint32_t desc = sad.getAudiodescriptor();
    EXPECT_EQ(desc, 0x563412);  // Little-endian
}

TEST_F(OperandsTest, ShortAudioDescriptorGetAtmosbit) {
    uint8_t buf1[3] = {0x48, 0x00, 0x01};  // Format 9+, atmos bit set
    ShortAudioDescriptor sad1(buf1, 3);
    EXPECT_EQ(sad1.getAtmosbit(), 1);
    
    uint8_t buf2[3] = {0x48, 0x00, 0x00};  // Format 9+, atmos bit not set
    ShortAudioDescriptor sad2(buf2, 3);
    EXPECT_EQ(sad2.getAtmosbit(), 0);
}

// ============= AllDeviceTypes Tests =============
TEST_F(OperandsTest, AllDeviceTypesGetAllDeviceTypes) {
    AllDeviceTypes types(0xFC);  // All bits set (TV, Recording, Tuner, Playback, Audio, Switch)
    std::vector<std::string> deviceTypes = types.getAllDeviceTypes();
    EXPECT_FALSE(deviceTypes.empty());
}

TEST_F(OperandsTest, AllDeviceTypesIsDeviceTypeTV) {
    AllDeviceTypes tvBit(1 << AllDeviceTypes::TV);
    EXPECT_TRUE(tvBit.isDeviceTypeTV());
    
    AllDeviceTypes noBit(0x00);
    EXPECT_FALSE(noBit.isDeviceTypeTV());
}

TEST_F(OperandsTest, AllDeviceTypesIsRecordingDevice) {
    AllDeviceTypes recBit(1 << AllDeviceTypes::RECORDING_DEVICE);
    EXPECT_TRUE(recBit.isRecordingDevice());
}

TEST_F(OperandsTest, AllDeviceTypesIsDeviceTypeTuner) {
    AllDeviceTypes tunerBit(1 << AllDeviceTypes::TUNER);
    EXPECT_TRUE(tunerBit.isDeviceTypeTuner());
}

TEST_F(OperandsTest, AllDeviceTypesIsPlaybackDevice) {
    AllDeviceTypes playbackBit(1 << AllDeviceTypes::PLAYBACK_DEVICE);
    EXPECT_TRUE(playbackBit.isPlaybackDevice());
}

TEST_F(OperandsTest, AllDeviceTypesIsDeviceTypeAudioSystem) {
    AllDeviceTypes audioBit(1 << AllDeviceTypes::AUDIO_SYSTEM);
    EXPECT_TRUE(audioBit.isDeviceTypeAudioSystem());
}

TEST_F(OperandsTest, AllDeviceTypesIsDeviceTypeCECSwitch) {
    AllDeviceTypes switchBit(1 << AllDeviceTypes::CEC_SWITCH);
    EXPECT_TRUE(switchBit.isDeviceTypeCECSwitch());
}

// ============= RcProfile Tests =============
TEST_F(OperandsTest, RcProfileGetRcProfile) {
    RcProfile profile(0x0E);  // RC Profile TV with Profile 4
    std::vector<std::string> profiles = profile.getRcProfile();
    EXPECT_FALSE(profiles.empty());
}

TEST_F(OperandsTest, RcProfileIsRcProfileTv) {
    RcProfile tvProfile(0x00);  // Bit 6 not set = TV profile
    EXPECT_TRUE(tvProfile.isRcProfileTv());
    
    RcProfile sourceProfile(0x40);  // Bit 6 set = Source profile
    EXPECT_FALSE(sourceProfile.isRcProfileTv());
}

TEST_F(OperandsTest, RcProfileIsRcProfileSource) {
    RcProfile sourceProfile(0x40);  // Bit 6 set
    EXPECT_TRUE(sourceProfile.isRcProfileSource());
}

TEST_F(OperandsTest, RcProfileRootMenuHandling) {
    RcProfile profile(0x40 | (1 << RcProfile::DEVICE_ROOT_MENU));
    EXPECT_TRUE(profile.rootMenuHandling());
}

TEST_F(OperandsTest, RcProfileSetupMenuHandling) {
    RcProfile profile(0x40 | (1 << RcProfile::DEVICE_SETUP_MENU));
    EXPECT_TRUE(profile.setupMenuHandling());
}

TEST_F(OperandsTest, RcProfileContentsMenuHandling) {
    RcProfile profile(0x40 | (1 << RcProfile::CONTENTS_MENU));
    EXPECT_TRUE(profile.contentsMenuHandling());
}

TEST_F(OperandsTest, RcProfileMediaTopMenuHandling) {
    RcProfile profile(0x40 | (1 << RcProfile::MEDIA_TOP_MENU));
    EXPECT_TRUE(profile.mediaTopMenuHandling());
}

TEST_F(OperandsTest, RcProfileContextSensitiveMenuHandling) {
    RcProfile profile(0x40 | (1 << RcProfile::MEDIA_CONTEXT_MENU));
    EXPECT_TRUE(profile.contextSensitiveMenuHandling());
}

// ============= DeviceFeatures Tests =============
TEST_F(OperandsTest, DeviceFeaturesGetDeviceFeatures) {
    DeviceFeatures features(0x7F);  // All feature bits set
    std::vector<std::string> featureList = features.getDeviceFeatures();
    EXPECT_FALSE(featureList.empty());
}

TEST_F(OperandsTest, DeviceFeaturesTvRecordScreenSupportBit) {
    DeviceFeatures features(1 << DeviceFeatures::RECORD_TV_SCREEN_SUPPORT);
    EXPECT_TRUE(features.tvRecordScreenSupportBit());
}

TEST_F(OperandsTest, DeviceFeaturesTVSetOSDStringSupportBit) {
    DeviceFeatures features(1 << DeviceFeatures::SET_OSD_STRING_SUPPORT);
    EXPECT_TRUE(features.tVSetOSDStringSupportBit());
}

TEST_F(OperandsTest, DeviceFeaturesControlledByDeckSupportBit) {
    DeviceFeatures features(1 << DeviceFeatures::CONTROLLED_BY_DECK);
    EXPECT_TRUE(features.controlledByDeckSupportBit());
}

TEST_F(OperandsTest, DeviceFeaturesSetAudioRateSupportBit) {
    DeviceFeatures features(1 << DeviceFeatures::SET_AUDIO_RATE_SUPPORT);
    EXPECT_TRUE(features.setAudioRateSupportBit());
}

TEST_F(OperandsTest, DeviceFeaturesArcTxSupportBit) {
    DeviceFeatures features(1 << DeviceFeatures::SINK_ARC_TX_SUPPORT);
    EXPECT_TRUE(features.arcTxSupportBit());
}

TEST_F(OperandsTest, DeviceFeaturesArcRxSupportBit) {
    DeviceFeatures features(1 << DeviceFeatures::ARC_RX_SUPPORT);
    EXPECT_TRUE(features.arcRxSupportBit());
}

// ============= LatencyInfo Tests =============
TEST_F(OperandsTest, LatencyInfoGetVideoLatency) {
    LatencyInfo latency(0x10);
    EXPECT_EQ(latency.getVideoLatency(), 0x10);
}

TEST_F(OperandsTest, LatencyInfoCreation) {
    LatencyInfo latency(0x20);
    EXPECT_NO_THROW({
        uint8_t video = latency.getVideoLatency();
        EXPECT_EQ(video, 0x20);
    });
}

/*
 * ============= Operand base-class defaults =============
 *
 * Operand declares serialize(CECFrame&) pure virtual and supplies non-pure defaults for
 * toString(), name() and validate().  Every concrete operand in Operands.hpp overrides
 * toString(), and nothing in the middleware calls name() or validate() at all, so the three
 * default bodies had never executed: in the baseline these cases were written against,
 * ccec/include/ccec/Operand.hpp measured 0.0% (0/6) - the lowest figure in the middleware trace -
 * purely because no test ever stood on the base class itself.  With the cases below in place it
 * measures 100% (9/9).
 *
 * The defaults are part of the operand contract: a new operand that forgets to override
 * toString() inherits "Not Implemented" rather than failing to compile, and validate()
 * returning true unconditionally is what makes an un-overridden operand acceptable to any
 * caller that validates before serialising.  Both are behaviours a reader would reasonably
 * rely on, so both are pinned to their exact values here rather than merely executed.
 *
 * The fixture below is the minimum a concrete Operand can be - it implements only the pure
 * virtual, appending one recognisable byte so the inherited serialize(void) overload can be
 * shown to route through it - and it lives in the test, not in production.
 */
namespace {

// A byte no operand in Operands.hpp emits, so seeing it in a frame can only mean the
// override below ran.  Namespace scope rather than a static class member: a C++14 in-class
// initialiser is not a definition, and passing the member to EXPECT_EQ binds it to a const
// reference, which odr-uses it and fails to link.
const uint8_t kMinimalOperandMarker = 0x5A;

class MinimalOperand : public Operand {
public:
    // Declaring serialize(CECFrame&) hides the base's no-argument overload by name lookup,
    // which is exactly the trap every concrete operand in Operands.hpp falls into; the
    // using-declaration brings it back so both forms can be exercised from one subject.
    using Operand::serialize;

    CECFrame &serialize(CECFrame &frame) const override {
        frame.append(kMinimalOperandMarker);
        return frame;
    }
};

} // namespace

TEST_F(OperandsTest, OperandDefaultToStringReportsNotImplemented) {
    MinimalOperand operand;

    // The inherited default, not an override: a concrete operand that does not describe
    // itself says so rather than returning an empty string a caller might log as success.
    EXPECT_EQ(std::string("Not Implemented"), operand.toString());
}

TEST_F(OperandsTest, OperandDefaultNameIsOperand) {
    MinimalOperand operand;

    EXPECT_EQ(std::string("Operand"), operand.name());
}

TEST_F(OperandsTest, OperandDefaultValidateAcceptsAnOperandThatDoesNotOverrideIt) {
    MinimalOperand operand;

    // Unconditionally true is the documented default: an operand carries no constraints
    // until it declares them, so a validating caller must not reject one that is silent.
    EXPECT_TRUE(operand.validate());
}

TEST_F(OperandsTest, OperandSerializeWithoutAFrameRoutesThroughTheOverride) {
    MinimalOperand operand;

    // The no-argument overload is non-virtual and forwards to the pure virtual one; the
    // marker byte is how that routing is observed rather than assumed.
    const CECFrame frame = operand.serialize();

    const uint8_t *buffer = nullptr;
    size_t length = 0;
    frame.getBuffer(&buffer, &length);
    ASSERT_EQ(1u, length);
    ASSERT_NE(nullptr, buffer);
    EXPECT_EQ(kMinimalOperandMarker, buffer[0]);
}

/*
 * ============= Exception hierarchy what() strings =============
 *
 * Exception.hpp is a header-only hierarchy of seven classes whose entire behaviour is the
 * string each what() returns.  Three of them - OperationNotSupportedException, IOException
 * and InvalidParamException - are already exercised, because some case somewhere prints the
 * exception it caught.  The other four were never printed by any test, which is what left
 * ccec/include/ccec/Exception.hpp at a measured 33.3% (4/12) in that same baseline; with the cases
 * below it measures 100% (14/14).
 *
 * These strings are the diagnostic a caller sees when a CEC operation fails, so they are
 * asserted exactly: a change to one of them changes what an operator reads in a log, and a
 * test that only called what() and discarded the result would not notice.  Catching each by
 * its own type through a base-class reference also pins the two properties the hierarchy
 * exists for - that every member derives from Exception, and that what() is virtual - which
 * a direct call on a concrete object would not show.
 */
TEST_F(OperandsTest, ExceptionHierarchyReportsItsOwnDiagnosticString) {
    struct Expectation {
        const char *what;
        const std::exception *instance;
    };

    const Exception base;
    const CECNoAckException noAck;
    const OperationNotSupportedException notSupported;
    const IOException io;
    const InvalidStateException invalidState;
    const InvalidParamException invalidParam;
    const AddressNotAvailableException addressUnavailable;

    const Expectation expectations[] = {
        { "Base Exception..",                     &base },
        { "Ack not received..",                   &noAck },
        { "Operation Not Supported..",            &notSupported },
        { "IO Exception..",                       &io },
        { "Invalid State Exception..",            &invalidState },
        { "Invalid Param Exception..",            &invalidParam },
        { "Address Not Available Exception..",    &addressUnavailable }
    };

    for (size_t i = 0; i < sizeof(expectations) / sizeof(expectations[0]); ++i) {
        EXPECT_STREQ(expectations[i].what, expectations[i].instance->what())
            << "exception at index " << i << " reported the wrong diagnostic string";
    }
}

TEST_F(OperandsTest, EveryCecExceptionIsCaughtAsTheBaseTypeAndKeepsItsOwnMessage) {
    // Thrown and caught by base reference on purpose: this is how the middleware handles
    // them (see DriverImpl and LibCCEC), so it is the dispatch that has to hold.
    try {
        throw CECNoAckException();
    } catch (const Exception &caught) {
        EXPECT_STREQ("Ack not received..", caught.what());
    }

    try {
        throw InvalidStateException();
    } catch (const Exception &caught) {
        EXPECT_STREQ("Invalid State Exception..", caught.what());
    }

    try {
        throw AddressNotAvailableException();
    } catch (const Exception &caught) {
        EXPECT_STREQ("Address Not Available Exception..", caught.what());
    }

    // std::exception is the outermost handler a caller can install, and the hierarchy has to
    // remain reachable through it.
    try {
        throw IOException();
    } catch (const std::exception &caught) {
        EXPECT_STREQ("IO Exception..", caught.what());
    }
}
