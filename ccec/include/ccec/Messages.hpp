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

/**
 * @defgroup HDMI_CEC_MSG_N_FRAME HDMI-CEC Messages and Frames
 * The CECFrame is a byte buffer that provides access to raw CEC bytes. CECFrame is guaranteed to be a
 * complete CEC Message that has the necessary data blocks:
 * @n @n
 * - The Header Data block (The byte that contains the initiator and destination address)
 * - The OpCode Data Block (The byte that contains the opcode).
 * - The Operand Data Block.(The bytes that contains the operands).
 *
 * In most cases application need not access CECFrame directly, but manipulate the raw bytes through the Message API.
 * @n @n
 * The Message API allows the application to send or receive high-level CEC message construct instead of raw bytes.
 * Basically for each CEC message (such as ActiveSource), there is a C++ class implementation representing it.
 * Each message class provides necessary getter and setter methods to access the properties of each message.
 * @ingroup HDMI_CEC
 *
 * @defgroup HDMI_CEC_MSG_N_FRAME_CLASSES HDMI-CEC Messages and Frames Classes
 * Described the details of High Level Class diagram and its functionalities.
 * @ingroup HDMI_CEC_MSG_N_FRAME
 *
 * @defgroup HDMI_CEC_MULTI_SYNC HDMI-CEC Messages - Asynchronous Vs. Synchronous
 * When messages converge on the logical buses, they are queued for sending opportunities on the physical bus.
 * The waiting time for such send to complete, though short in most cases, can be problematic to some interactive
 * real-time applications. It is recommended that the applications always send CEC messages asynchronously via
 * the Connection API and use the listener APIs to monitor response messages or device state changes.
 * The CEC library offers abundant APIs to facilitate such asynchronous implementation and the application is
 * encouraged to make full use of them.
 * @n @n
 * Given the vast variance of HDMI-CEC support from the off-the-self media devices, it is not recommended that
 * application wait for the response from a destination device. Even if the request message is sent out successfully,
 * the destination device may choose to ignore the request. The recommended approach is again to send the request
 * asynchronously and use the listener to monitor responses.
 * @n @n
 * Overall, given the asynchronous nature of HDMI-CEC, application should always opt to use Asynchronous APIs
 * as first choice. And for same reasons, the RDK CEC library offers only limited support for Synchronous APIs.
 * @ingroup HDMI_CEC_MSG_N_FRAME
 */

/**
* @defgroup hdmicec
* @{
* @defgroup ccec
* @{
**/


#ifndef HDMI_CCEC_MESSAGES_HPP_
#define HDMI_CCEC_MESSAGES_HPP_

#include <stdint.h>

#include <iostream>
#include "CCEC.hpp"
#include "Assert.hpp"
#include "ccec/CECFrame.hpp"
#include "DataBlock.hpp"
#include "OpCode.hpp"
#include "Operands.hpp"
#include "ccec/Util.hpp"

CCEC_BEGIN_NAMESPACE
/**
 * @brief The Message API allows the application to send or receive high-level CEC message construct instead of raw bytes.
 *
 * Basically for each CEC message (such as ActiveSource), there is a C++ class implementation representing it.
 * Each message class provides necessary getter and setter methods to access the properties of each message.
 * @ingroup HDMI_CEC_MSG_N_FRAME_CLASSES
 */
class ActiveSource : public DataBlock 
{
public:
    Op_t opCode(void) const {return ACTIVE_SOURCE;}

	ActiveSource(const PhysicalAddress &physicalAddress) : physicalAddress(physicalAddress) {
    }

	ActiveSource(const CECFrame &frame, int startPos = 0) 
    : physicalAddress(frame, startPos)
    {
    }
	CECFrame &serialize(CECFrame &frame) const {
        CCEC_LOG( LOG_DEBUG, "%s \n",physicalAddress.toString().c_str());
        return physicalAddress.serialize(frame);
	}

    void print(void) const {
        CCEC_LOG( LOG_DEBUG, "%s : %s  : %s \n",GetOpName(opCode()),physicalAddress.name().c_str(),physicalAddress.toString().c_str());
    }
public:
    PhysicalAddress physicalAddress;
};

class ImageViewOn : public DataBlock
{
public:
    Op_t opCode(void) const {return IMAGE_VIEW_ON;}
};


class TextViewOn : public DataBlock
{
public:
    Op_t opCode(void) const {return TEXT_VIEW_ON;}
};

class InActiveSource : public DataBlock 
{
public:
    Op_t opCode(void) const {return INACTIVE_SOURCE;}

	InActiveSource(const PhysicalAddress &physicalAddress) : physicalAddress(physicalAddress) {}

	InActiveSource(const CECFrame &frame, int startPos = 0) 
    : physicalAddress(frame, startPos)
    {
    }

	CECFrame &serialize(CECFrame &frame) const {
        CCEC_LOG( LOG_DEBUG, "%s \n",physicalAddress.toString().c_str());
        return physicalAddress.serialize(frame);
	}

    void print(void) const {
        CCEC_LOG( LOG_DEBUG, "%s : %s : %s  \n",GetOpName(opCode()),physicalAddress.name().c_str(),physicalAddress.toString().c_str());
    }
public:
    PhysicalAddress physicalAddress;
};

class RequestActiveSource : public DataBlock
{
public:
    Op_t opCode(void) const {return REQUEST_ACTIVE_SOURCE;}
};

class Standby : public DataBlock
{
public:
    Op_t opCode(void) const {return STANDBY;}
};

class GetCECVersion : public DataBlock
{
public:
    Op_t opCode(void) const {return GET_CEC_VERSION;}
};

class CECVersion : public DataBlock
{
public:
    Op_t opCode(void) const {return CEC_VERSION;}

    CECVersion(const Version &version) : version(version) {}

    CECVersion(const CECFrame &frame, int startPos = 0)
    : version(frame, startPos)
    {
    }

	CECFrame &serialize(CECFrame &frame) const {
        return version.serialize(frame);
	}

    void print(void) const {
        CCEC_LOG( LOG_DEBUG, "Version : %s \n",version.toString().c_str());
    }

    Version version;
};

class SetMenuLanguage : public DataBlock
{

public:
    Op_t opCode(void) const {return SET_MENU_LANGUAGE;}

    SetMenuLanguage(const Language &language) : language(language) {};

    SetMenuLanguage(const CECFrame &frame, int startPos = 0)
    : language(frame, startPos)
    {
    }

	CECFrame &serialize(CECFrame &frame) const {
	    return language.serialize(frame);
	}

    void print(void) const {
        CCEC_LOG( LOG_DEBUG, "Language : %s \n",language.toString().c_str());
    }

    Language language;
};

class GetMenuLanguage: public DataBlock
{
public:
    Op_t opCode(void) const {return GET_MENU_LANGUAGE;}
};

class GiveOSDName: public DataBlock
{
public:
    Op_t opCode(void) const {return GIVE_OSD_NAME;}
};

class SetOSDName: public DataBlock
{
public:
    Op_t opCode(void) const {return SET_OSD_NAME;}

    SetOSDName(const OSDName &osdName) : osdName(osdName) {};

    SetOSDName(const CECFrame &frame, int startPos = 0)
    : osdName(frame, startPos)
    {
    }

	CECFrame &serialize(CECFrame &frame) const {
	    return osdName.serialize(frame);
	}

    void print(void) const {
        CCEC_LOG( LOG_DEBUG,"OSDName : %s\n",osdName.toString().c_str());
    }

    OSDName osdName;
};

class SetOSDString : public DataBlock
{
public:
    Op_t opCode(void) const {return SET_OSD_STRING;}

    SetOSDString(const OSDString &osdString) : osdString(osdString) {};

    SetOSDString(const CECFrame &frame, int startPos = 0)
    : osdString(frame, startPos)
    {
    }

	CECFrame &serialize(CECFrame &frame) const {
	    return osdString.serialize(frame);
	}

    void print(void) const {
        CCEC_LOG( LOG_DEBUG,"OSDString : %s\n",osdString.toString().c_str());
    }

    OSDString osdString;
};

class GivePhysicalAddress : public DataBlock
{
public:
    Op_t opCode(void) const {return GIVE_PHYSICAL_ADDRESS;}
};

class ReportPhysicalAddress : public DataBlock
{
public:
    Op_t opCode(void) const {return REPORT_PHYSICAL_ADDRESS;}

    ReportPhysicalAddress(const PhysicalAddress &physicalAddress, const DeviceType &deviceType)
    : physicalAddress(physicalAddress), deviceType(deviceType) {
    }

    ReportPhysicalAddress(const CECFrame &frame, int startPos = 0)
    : physicalAddress(frame, startPos), deviceType(frame, startPos + PhysicalAddress::MAX_LEN)
    {
    }

	CECFrame &serialize(CECFrame &frame) const {
		return deviceType.serialize(physicalAddress.serialize(frame));
	}

    void print(void) const {
        CCEC_LOG( LOG_DEBUG,"Physical Address : %s\n",physicalAddress.toString().c_str());
        CCEC_LOG( LOG_DEBUG,"Device Type : %s\n",deviceType.toString().c_str());
    }

	PhysicalAddress physicalAddress;
	DeviceType deviceType;
};

class GiveDeviceVendorID : public DataBlock
{
public:
    Op_t opCode(void) const {return GIVE_DEVICE_VENDOR_ID;}
};

class DeviceVendorID : public DataBlock
{
public:
    Op_t opCode(void) const {return DEVICE_VENDOR_ID;}

    DeviceVendorID(const VendorID &vendorId) : vendorId(vendorId) {}

    DeviceVendorID(const CECFrame &frame, int startPos = 0)
    : vendorId(frame, startPos)
    {
    }

	CECFrame &serialize(CECFrame &frame) const {
		return vendorId.serialize(frame);
	}

    void print(void) const {
        CCEC_LOG( LOG_DEBUG,"VendorID : %s\n",vendorId.toString().c_str());
    }

    VendorID vendorId;
};

class GiveDevicePowerStatus : public DataBlock
{
public:
    Op_t opCode(void) const {return GIVE_DEVICE_POWER_STATUS;}
};

class ReportPowerStatus : public DataBlock
{
public:
    Op_t opCode(void) const {return REPORT_POWER_STATUS;}

    ReportPowerStatus(const PowerStatus &status) : status(status) {}

    ReportPowerStatus(const CECFrame &frame, int startPos = 0)
    : status(frame, startPos)
    {
    }

	CECFrame &serialize(CECFrame &frame) const {
		return status.serialize(frame);
	}

    void print(void) const {
        CCEC_LOG( LOG_DEBUG,"Power Status: %s",status.toString().c_str());
    }

    PowerStatus status;
};

class Abort : public DataBlock
{
public:
    Op_t opCode(void) const {return ABORT;}
};

class FeatureAbort: public DataBlock
{
public:
    Op_t opCode(void) const {return FEATURE_ABORT;}

    FeatureAbort(const OpCode &feature, const AbortReason &reason) : feature(feature), reason(reason) {}

    FeatureAbort(const CECFrame &frame, int startPos = 0)
    : feature(frame, startPos), reason(frame, startPos + OpCode::MAX_LEN)
    {
    }

	CECFrame &serialize(CECFrame &frame) const {
		return reason.serialize(feature.serialize(frame));
	}

    void print(void) const {
        CCEC_LOG( LOG_DEBUG,"Abort For Feature : %s\n",feature.toString().c_str());
        CCEC_LOG( LOG_DEBUG,"Abort Reason : %s\n",reason.toString().c_str());
    }

    OpCode feature;
    AbortReason reason;
};

class RoutingChange: public DataBlock
{
public:
    Op_t opCode(void) const {return ROUTING_CHANGE;}

    RoutingChange(const PhysicalAddress &from, const PhysicalAddress &to) : from(from), to(to) {}

    RoutingChange(const CECFrame &frame, int startPos = 0)
    : from(frame, startPos), to(frame, startPos + PhysicalAddress::MAX_LEN)
    {
    }

	CECFrame &serialize(CECFrame &frame) const {
		return to.serialize(from.serialize(frame));
	}

    void print(void) const {
        CCEC_LOG( LOG_DEBUG,"Routing Change From : %s\n",from.toString().c_str());
        CCEC_LOG( LOG_DEBUG,"Routing Change to : %s\n",to.toString().c_str());
    }

    PhysicalAddress from;
    PhysicalAddress to;
};

class RoutingInformation: public DataBlock
{
public:
    Op_t opCode(void) const {return ROUTING_INFORMATION;}

    RoutingInformation(const PhysicalAddress &toSink) : toSink(toSink) {}

    RoutingInformation(const CECFrame &frame, int startPos = 0)
    : toSink(frame, startPos)
    {
    }

	CECFrame &serialize(CECFrame &frame) const {
		return toSink.serialize(frame);
	}

    void print(void) const {
        CCEC_LOG( LOG_DEBUG,"Routing Information to Sink : %s\n",toSink.toString().c_str());
    }

    PhysicalAddress toSink;
};


class SetStreamPath: public DataBlock
{
public:
    Op_t opCode(void) const {return SET_STREAM_PATH;}

    SetStreamPath(const PhysicalAddress &toSink) : toSink(toSink) {}

    SetStreamPath(const CECFrame &frame, int startPos = 0)
    : toSink(frame, startPos)
    {
    }

	CECFrame &serialize(CECFrame &frame) const {
		return toSink.serialize(frame);
	}

    void print(void) const {
        CCEC_LOG( LOG_DEBUG,"Set Stream Path to Sink : %s\n",toSink.toString().c_str());
    }

    PhysicalAddress toSink;
};

class RequestShortAudioDescriptor : public DataBlock
{

public:
    Op_t opCode(void) const {return REQUEST_SHORT_AUDIO_DESCRIPTOR;}

      RequestShortAudioDescriptor(const std::vector<uint8_t> formatid, const std::vector<uint8_t> audioFormatCode, uint8_t number_of_descriptor = 1)
      {
	    uint8_t audioFormatIdCode;
	    numberofdescriptor = number_of_descriptor > 4 ? 4 : number_of_descriptor;
	    for (uint8_t i=0 ; i < numberofdescriptor ;i++)
	    {
		   audioFormatIdCode = (formatid[i] << 6) | ( (audioFormatCode[i])& 0x3f) ;
		   requestAudioFormat.push_back(RequestAudioFormat(audioFormatIdCode));
	    }
       }
	 /* called by the messaged_decoder */
     RequestShortAudioDescriptor(const CECFrame &frame, int startPos = 0)
     {
	uint8_t len = frame.length();
        numberofdescriptor = len > 4 ? 4:len;
        for (uint8_t i=0; i< numberofdescriptor ; i++)
        {
	   requestAudioFormat.push_back(RequestAudioFormat(frame,startPos + i ));
        }
     }
     /* called by the message encoder */
     CECFrame &serialize(CECFrame &frame) const {

	  for (uint8_t i=0; i < numberofdescriptor ; i++)
	  {
	  requestAudioFormat[i].serialize(frame);

	  }
	  return frame;
	}

     void print(void) const {
	    uint8_t i=0;
		for(i=0;i < numberofdescriptor;i++)
		{

                  CCEC_LOG( LOG_DEBUG,"audio format id %d audioFormatCode : %s\n",requestAudioFormat[i].getAudioformatId(),requestAudioFormat[i].toString().c_str());

		}
      }
     std::vector<RequestAudioFormat> requestAudioFormat ;
     uint8_t  numberofdescriptor;
};
class ReportShortAudioDescriptor : public DataBlock
{

public:
    Op_t opCode(void) const {return REPORT_SHORT_AUDIO_DESCRIPTOR;}

	      ReportShortAudioDescriptor( const std::vector <uint32_t> shortaudiodescriptor, uint8_t numberofdescriptor = 1)
	      {

	       	uint8_t bytes[3];
	        numberofdescriptor = numberofdescriptor > 4 ? 4 : numberofdescriptor;
	        for (uint8_t i=0; i < numberofdescriptor ;i++)
	        {
                  bytes[0] = (shortaudiodescriptor[i] & 0xF);
	          bytes[1] = ((shortaudiodescriptor[i] >> 8) & 0xF);
	          bytes[2] = ((shortaudiodescriptor[i] >> 16) & 0xF);
	          shortAudioDescriptor.push_back(ShortAudioDescriptor(bytes));

	        }
	      }
	       /* called by the messaged_decoder */
             ReportShortAudioDescriptor(const CECFrame &frame, int startPos = 0)
            {
               numberofdescriptor = (frame.length())/3;
               for (uint8_t i=0; i< numberofdescriptor ;i++)
	       {
	          shortAudioDescriptor.push_back(ShortAudioDescriptor(frame,startPos + i*3 ));
	       }
             }

	     /* called by the message encoder*/
	   CECFrame &serialize(CECFrame &frame) const {
		 for (uint8_t i=0; i < numberofdescriptor ; i++)
		 {
		 //just do the append of the stored cec bytes
		 shortAudioDescriptor[i].serialize(frame);

		 }
		 return frame;
	   }
           void print(void) const {
                for(uint8_t i=0;i < numberofdescriptor;i++)
                {
			CCEC_LOG( LOG_DEBUG," audioFormatCode : %s audioFormatCode %d Atmos = %d\n",shortAudioDescriptor[i].toString().c_str(),shortAudioDescriptor[i].getAudioformatCode(),shortAudioDescriptor[i].getAtmosbit());
		}
	   }
    std::vector <ShortAudioDescriptor> shortAudioDescriptor ;
    uint8_t numberofdescriptor;
};
class SystemAudioModeRequest : public DataBlock
{

public:
    Op_t opCode(void) const {return SYSTEM_AUDIO_MODE_REQUEST;}
	SystemAudioModeRequest(const PhysicalAddress &physicaladdress = {0xf,0xf,0xf,0xf} ): physicaladdress(physicaladdress) {}
	 /* called by the messaged_decoder */
	SystemAudioModeRequest(const CECFrame &frame, int startPos = 0):physicaladdress(frame, startPos)
        {
           if (frame.length() == 0 )
	   {
              physicaladdress= PhysicalAddress((uint8_t) 0xf,(uint8_t) 0xf,(uint8_t)0xf,(uint8_t)0xf);
	   }
        }
      CECFrame &serialize(CECFrame &frame) const {
        if ( (physicaladdress.getByteValue(3) == 0xF) && (physicaladdress.getByteValue(2) == 0xF) && (physicaladdress.getByteValue(1) == 0xF) &&  (physicaladdress.getByteValue(0) == 0xF))
	{
	    return frame;
	 } else{
                return physicaladdress.serialize(frame);
	 }
	 }
     void print(void) const {
        CCEC_LOG( LOG_DEBUG,"Set SystemAudioModeRequest : %s\n",physicaladdress.toString().c_str());
    }
  PhysicalAddress physicaladdress;
};
class SetSystemAudioMode: public DataBlock
{
public:
    Op_t opCode(void) const {return SET_SYSTEM_AUDIO_MODE;}

	SetSystemAudioMode( const SystemAudioStatus &status ) : status(status) { }

	SetSystemAudioMode(const CECFrame &frame, int startPos = 0) : status(frame, startPos)
       {
       }
	CECFrame &serialize(CECFrame &frame) const {
		return status.serialize(frame);
	}

	SystemAudioStatus status;
};

class GiveAudioStatus : public DataBlock
{
public:
    Op_t opCode(void) const {return GIVE_AUDIO_STATUS;}
};
class ReportAudioStatus: public DataBlock
{
public:
    Op_t opCode(void) const {return REPORT_AUDIO_STATUS;}

    ReportAudioStatus( const AudioStatus &status ) : status(status) { }
    ReportAudioStatus(const CECFrame &frame, int startPos = 0):status(frame, startPos)
    {
    }
    CECFrame &serialize(CECFrame &frame) const {
	return status.serialize(frame);
    }
    AudioStatus status;
};

class UserControlPressed: public DataBlock
{
public:
    Op_t opCode(void) const {return USER_CONTROL_PRESSED;}

	UserControlPressed( const UICommand &command ) : uiCommand(command) { }
    UserControlPressed(const CECFrame &frame, int startPos = 0):uiCommand(frame, startPos)
	{
	}

	CECFrame &serialize(CECFrame &frame) const {
		return uiCommand.serialize(frame);
	}

	UICommand uiCommand;
};

class UserControlReleased: public DataBlock
{
public:
    Op_t opCode(void) const {return USER_CONTROL_RELEASED;}
};


class Polling : public DataBlock
{
public:
    Op_t opCode(void) const {return POLLING;}
};

class RequestArcInitiation:public DataBlock
{
  public:
    Op_t opCode(void) const {return REQUEST_ARC_INITIATION;}

};

class ReportArcInitiation:public DataBlock
{
  public:
    Op_t opCode(void) const {return REPORT_ARC_INITIATED;}

};
class RequestArcTermination:public DataBlock
{
  public:
    Op_t opCode(void) const {return REQUEST_ARC_TERMINATION;}

};
class ReportArcTermination:public DataBlock
{
  public:
    Op_t opCode(void) const {return REPORT_ARC_TERMINATED;}

};
class InitiateArc:public DataBlock
{
  public:
    Op_t opCode(void) const {return INITIATE_ARC;}
};
class TerminateArc:public DataBlock
{
  public:
    Op_t opCode(void) const {return TERMINATE_ARC;}
};
class GiveFeatures : public DataBlock
{
public:
    Op_t opCode(void) const {return GIVE_FEATURES;}
};
class ReportFeatures : public DataBlock
{

  public:
    Op_t opCode(void) const {return REPORT_FEATURES;}
        ReportFeatures(const Version &version,const AllDeviceTypes &allDeviceTypes,const std::vector<RcProfile> rc_Profile,std::vector<DeviceFeatures> device_Features) : version(version), allDeviceTypes(allDeviceTypes)
        {
                for(uint8_t i = 0; i < rc_Profile.size(); i++){
                       rcProfile.push_back(RcProfile(rc_Profile[i]));
                }

                for(uint8_t i = 0; i < device_Features.size(); i++){
                    deviceFeatures.push_back(DeviceFeatures(device_Features[i]));
                }
        }

        /* called by the messaged_decoder */
        ReportFeatures(const CECFrame &frame, int startPos = 0) : version(frame, startPos = 0) , allDeviceTypes(frame, startPos+Version::MAX_LEN) {
                const uint8_t *buf = 0;
                size_t frameLen = 0, rc_len = 1, features_len = 1;

                frame.getBuffer(&buf, &frameLen);

                //The frame is stored in buffer
                CCEC_LOG( LOG_INFO, "Features Buffer ----> Frame Length:  %d\n",frameLen);
                for(size_t i = 0; i < frameLen ;i++){
                        CCEC_LOG( LOG_INFO, "%0x :\n",buf[i]);
                }

                //[RC Profile] length is calculated based on Extension bit
                for(size_t i = 2; i< frame.length(); i++) {
                    if(buf[i] & 0x80){
                        rc_len++;
                    }
                    else{
                        break;
                    }
                }

                //[RC Profile] and [Device Features] are Variable Length
                //Bit 7 of each byte corresponds to Extension bit
                //If Bit7 = 1, then next byte corresponds to same Operand. If Bit7 = 0, then Operand ends with that byte

                rcProfile.push_back(RcProfile(frame, startPos + 2, rc_len));

                //[Device Features] length is calculated
                for(size_t i = rc_len + 2; i < frame.length(); i++){
                    if(buf[i] & 0x80){
                        features_len++;
                    }
                    else{
                        break;
                    }
                }
                deviceFeatures.push_back(DeviceFeatures(frame, startPos + rc_len + 2 , features_len));
        }

        CECFrame &serialize(CECFrame &frame) const {
            allDeviceTypes.serialize(version.serialize(frame));

            for (uint8_t i = 0; i < rcProfile.size() ; i++) {
                rcProfile[i].serialize(frame);
            }

            for (uint8_t i = 0; i < deviceFeatures.size() ; i++) {
                deviceFeatures[i].serialize(frame);
            }

            return frame;
        }

          void print(void) const {
                std::vector<std::string> all_device_types, rc_profile, device_features;
                std::vector<uint8_t> rc_profile_val, device_features_val;

                //Operand - [CEC Version]
                CCEC_LOG( LOG_INFO, "Version : %s \n",version.toString().c_str());


                //Operand - [All Device Types]
                all_device_types = allDeviceTypes.getAllDeviceTypes();
                CCEC_LOG( LOG_INFO, "All Device types:\n");
                for(size_t i = 0; i < all_device_types.size(); i++){
                    CCEC_LOG( LOG_INFO, "%s\t", all_device_types[i].c_str());
                }

                CCEC_LOG( LOG_INFO, "Device Type TV: %d, Recording Device: %d, Tuner: %d, Playback Device: %d, Audio Sytem: %d, CEC Switch: %d", \
                allDeviceTypes.isDeviceTypeTV(), allDeviceTypes.isRecordingDevice(), allDeviceTypes.isDeviceTypeTuner(), allDeviceTypes.isPlaybackDevice(),\
                allDeviceTypes.isDeviceTypeAudioSystem(), allDeviceTypes.isDeviceTypeCECSwitch());

                //Operand - [RC Profile]
                for(size_t i = 0; i < rcProfile.size(); i++){
                    rc_profile = rcProfile[i].getRcProfile();
                    CCEC_LOG(LOG_INFO, "RC Profile TV: %d, RC Profile Source: %d", rcProfile[i].isRcProfileTv(), rcProfile[i].isRcProfileSource());
                    if(rcProfile[i].isRcProfileSource()){
                            CCEC_LOG(LOG_INFO, "Source can handle Device Root Menu: %d, Source can handle Device Setup Menu: %d, Source can handle Contents Menu: %d, Source can handle Media Top Menu: %d, Source can handle Media Context-Sensitive Menu: %d", rcProfile[i].rootMenuHandling(),rcProfile[i].setupMenuHandling(),rcProfile[i].contentsMenuHandling(),rcProfile[i].mediaTopMenuHandling(),rcProfile[i].contextSensitiveMenuHandling());
                    }
                }
                CCEC_LOG( LOG_INFO, "RC Profiles:\n");
                for(size_t i = 0; i < rc_profile.size(); i++){
                        CCEC_LOG( LOG_INFO, " %s\t", rc_profile[i].c_str());
                }

                //CEC Byte Values corresponding to RC Profile Operand
                for(size_t i = 0; i < rcProfile.size(); i++){
                    rc_profile_val = rcProfile[i].getRcProfileVal();
                }
                CCEC_LOG( LOG_INFO, "RC Profile Byte Values:\n");
                for(size_t i = 0; i < rc_profile_val.size(); i++){
                        CCEC_LOG( LOG_INFO, " %0x\t", rc_profile_val[i]);
                }


                //Operand - [Device Features]
                for(size_t i = 0; i < deviceFeatures.size(); i++){
                    device_features = deviceFeatures[i].getDeviceFeatures();
                    CCEC_LOG( LOG_INFO, "Tv Record Screen Support: %d, TV SetOSD string Support: %d, Supports being controlled by Deck Control:%d,\
                    Source supports Set Audio Rate: %d, Sink supports ARC Tx: %d, Source supports ARC Rx: %d", deviceFeatures[i].tvRecordScreenSupportBit(), \
                    deviceFeatures[i].tVSetOSDStringSupportBit(), deviceFeatures[i].controlledByDeckSupportBit(), deviceFeatures[i].setAudioRateSupportBit(), \
                    deviceFeatures[i].arcTxSupportBit() , deviceFeatures[i].arcRxSupportBit());
                }
                CCEC_LOG( LOG_INFO, "Device Features\n");
                for(size_t i = 0; i < device_features.size(); i++){
                        CCEC_LOG( LOG_INFO, " %s\t",device_features[i].c_str());
                }

                //CEC Byte Values corresponding to Device Features Operand
                for(size_t i = 0; i < deviceFeatures.size(); i++){
                    device_features_val = deviceFeatures[i].getDeviceFeaturesVal();
                }
                CCEC_LOG( LOG_INFO, "Device Features Byte values:\n");
                for(size_t i = 0; i < device_features_val.size(); i++){
                        CCEC_LOG( LOG_INFO, " %0x\t", device_features_val[i]);
                }

          }

        Version version;
        AllDeviceTypes allDeviceTypes;
        std::vector<RcProfile> rcProfile;
	std::vector<DeviceFeatures> deviceFeatures;
};
class RequestCurrentLatency : public DataBlock
{

  public:
    Op_t opCode(void) const {return REQUEST_CURRENT_LATENCY;}
        RequestCurrentLatency(const PhysicalAddress &physicaladdress = {0xf,0xf,0xf,0xf} ): physicaladdress(physicaladdress) {}
         /* called by the messaged_decoder */
        RequestCurrentLatency(const CECFrame &frame, int startPos = 0):physicaladdress(frame, startPos)
        {
           if (frame.length() == 0 )
           {
              physicaladdress= PhysicalAddress((uint8_t) 0xf,(uint8_t) 0xf,(uint8_t)0xf,(uint8_t)0xf);
           }
        }
        CECFrame &serialize(CECFrame &frame) const {
          if ( (physicaladdress.getByteValue(3) == 0xF) && (physicaladdress.getByteValue(2) == 0xF) && (physicaladdress.getByteValue(1) == 0xF) &&  (physicaladdress.getByteValue(0) == 0xF)) {
            return frame;
          }
	  else {
            return physicaladdress.serialize(frame);
          }
      }
      void print(void) const {
        CCEC_LOG( LOG_DEBUG,"RequestCurrentLatency : %s\n",physicaladdress.toString().c_str());
      }
  PhysicalAddress physicaladdress;
};
class ReportCurrentLatency : public DataBlock
{

  public:
    Op_t opCode(void) const {return REPORT_CURRENT_LATENCY;}
        ReportCurrentLatency(const PhysicalAddress &physicaladdress, uint8_t videoLatency, uint8_t latencyFlags, uint8_t audioOutputDelay = 0) : physicaladdress(physicaladdress) {

	    latencyInfo.push_back(LatencyInfo(videoLatency));
	    latencyInfo.push_back(LatencyInfo(latencyFlags));

	    /* Audio Output Delay is expected only if Latency Flags is equal to 3*/
            if(((latencyFlags) & 0x3) == 0x3){
                CCEC_LOG( LOG_DEBUG, "Audio Output compensated \n Audio Ouput Delay is %d ms\n", audioOutputDelay);
		latencyInfo.push_back(LatencyInfo(audioOutputDelay));
            }
	}
         /* called by the messaged_decoder */
        ReportCurrentLatency(const CECFrame &frame, int startPos = 0) :physicaladdress(frame, startPos) {
            uint8_t frame_len = frame.length();
            frame_len = frame_len > 5 ? 5 : frame_len ;
            latencyInfo.push_back(LatencyInfo(frame,startPos + 2, frame_len - 2));
        }


        CECFrame &serialize(CECFrame &frame) const {
	    physicaladdress.serialize(frame);
            for (uint8_t i=0; i < latencyInfo.size() ; i++) {
	        latencyInfo[i].serialize(frame);
            }

	    return frame;
        }

     void print(void) const {
        CCEC_LOG( LOG_DEBUG,"Physical address: %s \n LatencyInfo : \n",physicaladdress.toString().c_str());
        for(uint8_t i = 0 ; i < latencyInfo.size() ; i++) {
            CCEC_LOG( LOG_DEBUG,"Video Latency : %d, Latency Flags : %d, Audio output Delay : %d\n", latencyInfo[i].getVideoLatency(), latencyInfo[i].getLatencyFlags(), latencyInfo[i].getAudioOutputDelay() );
        }
     }
  PhysicalAddress physicaladdress;
  std::vector<LatencyInfo> latencyInfo;
};
#endif


/** @} */
/** @} */
