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
* @defgroup hdmicec
* @{
* @defgroup ccec
* @{
**/

#ifndef HDMI_CCEC_OPERANDS_HPP_
#define HDMI_CCEC_OPERANDS_HPP_
#include <stdint.h>

#include <cstring>
#include <cstdio>
#include <vector>
#include <bitset>
#include <string>

#include <sstream>

#include "CCEC.hpp"
#include "Assert.hpp"
#include "Operand.hpp"
#include "Util.hpp"
#include "ccec/CECFrame.hpp"
#include "ccec/Exception.hpp"

CCEC_BEGIN_NAMESPACE

class CECBytes : public Operand
{
protected:
    CECBytes(const uint8_t val) : str(1, val) {}
	CECBytes(const uint8_t *buf, size_t len) {
        if (buf && len) {
            for (size_t i = 0; i < len; i++) {
                str.push_back(buf[i]);
            }
        }
	}

	bool validate(void) const {
		return (str.size() && (str.size() <= getMaxLen()));
	}

	CECBytes(const CECFrame &frame, size_t startPos, size_t len) {
    	/*
    	 * For HDMI CEC definition, the [OSD Name] and [OSD STring] are always the one
    	 * and only one operands in the message. It is not clear if these strings are
    	 * null terminated.  Therefore, consume all remaining bytes in the frame as
    	 * CECBytes
    	 */
        const uint8_t *buf = 0;
        size_t frameLen = 0;
        frame.getBuffer(&buf, &frameLen);
        str.clear();
        len = ((startPos + len) > frameLen) ? frameLen - startPos : len;
        str.insert(str.begin(), buf + startPos, buf + startPos + len);
        if (!validate())
        {
            throw InvalidParamException();
        }
	}

public:
	CECFrame &serialize(CECFrame &frame) const {
		for (size_t i = 0; i < str.size(); i++) {
			frame.append(str[i]);
		}

		return frame;
	}
    using Operand::serialize;

	~CECBytes(void) {}

	virtual const std::string toString(void) const {
		std::stringstream stream;
		for (size_t i = 0; i < str.size(); i++) {
			stream << std::hex << (int)str[i];
		}
		return stream.str();
    };

	bool operator == (const CECBytes &in) const {
		return this->str == in.str;
	}

protected:
    std::vector<uint8_t> str;
    virtual size_t getMaxLen(void) const {
        return CECFrame::MAX_LENGTH;
    }
};

class OSDString : public CECBytes 
{
public:
	enum {
		MAX_LEN = 13,
	};

    OSDString(const char *str) : CECBytes((const uint8_t *)str, strlen(str)) {
        validate();
    }

    OSDString(const CECFrame &frame, size_t startPos) : CECBytes(frame, startPos, MAX_LEN) {
    }

	const std::string toString(void) const {
		return std::string(str.begin(), str.end());
	}
protected:
	size_t getMaxLen() const {return MAX_LEN;}
};

class OSDName : public CECBytes 
{
public:
	enum {
		MAX_LEN = 14,
	};

    OSDName(const char *str) : CECBytes((const uint8_t *)str, strlen(str)) {
        validate();
    }

    OSDName(const CECFrame &frame, size_t startPos) : CECBytes(frame, startPos, MAX_LEN) {
    }

	const std::string toString(void) const {
		return std::string(str.begin(), str.end());
	}
protected:
	size_t getMaxLen() const {return MAX_LEN;}
};

class AbortReason : public CECBytes 
{
public :
    enum {
        MAX_LEN = 1,
    };

	enum  {
		UNRECOGNIZED_OPCODE,
		NOT_IN_CORRECT_MODE_TO_RESPOND,
		CANNOT_OVERIDE_SOURCE,
		INVALID_OPERAND,
		REFUSED,
		UNABLE_TO_DETERMINE,
	};

	AbortReason(int reason) : CECBytes((uint8_t)reason) { }

	const std::string toString(void) const {
		static const char *names_[] = {
			"Unrecognized opcode",
			"Not in correct mode to respond",
			"Cannot provide source",
			"Invalid operand",
			"Refused",
			"Unable to determine",
		};

		if (validate())
		{
			return names_[str[0]];
		}
		else
		{
			CCEC_LOG(LOG_WARN,"Unknown abort reason:%x\n", str[0]);
			return "Unknown";
		}
	}

	bool validate(void) const {
		return (/*(str[0]>= UNRECOGNIZED_OPCODE) && */(str[0]<= UNABLE_TO_DETERMINE));
	}

	int toInt(void) const {
        return str[0];
    }

	AbortReason(const CECFrame &frame, size_t startPos) : CECBytes(frame, startPos, MAX_LEN) { } 


protected:
	size_t getMaxLen() const {return MAX_LEN;}

};

class DeviceType : public CECBytes 
{
public :
    enum {
        MAX_LEN = 1,
    };

	enum  {
		TV = 0x0,
		RECORDING_DEVICE,
		RESERVED,
		TUNER,
		PLAYBACK_DEVICE,
		AUDIO_SYSTEM,
		PURE_CEC_SWITCH,
		VIDEO_PROCESSOR,
	};

	DeviceType(int type) : CECBytes((uint8_t)type) {}

	const std::string toString(void) const {
		static const char *names_[] = {
				"TV",
				"Recording Device",
				"Reserved",
				"Tuner",
				"Playback Device",
				"Audio System",
				"Pure CEC Switch",
				"Video Processor",
		};

		if (validate())
		{
			return names_[str[0]];
		}
		else
		{
			CCEC_LOG(LOG_WARN,"Unknown device type:%x\n", str[0]);
			return "Unknown";
		}
	}

	bool validate(void) const {
		return (/*(str[0] >= TV) && */(str[0] <= VIDEO_PROCESSOR));
	}

	DeviceType(const CECFrame &frame, size_t startPos) : CECBytes(frame, startPos, MAX_LEN) {}

	~DeviceType(void) {}

protected:
	size_t getMaxLen() const {return MAX_LEN;}
};

class Language : public CECBytes 
{
public:
	enum {
		MAX_LEN = 3,
	};

    Language(const char *str) : CECBytes((const uint8_t*)str, strlen(str)) {
        Assert(strlen(str) <= MAX_LEN);
    }

    Language(const CECFrame &frame, size_t startPos) : CECBytes(frame, startPos, MAX_LEN) {
    }

	const std::string toString(void) const {
		return std::string(str.begin(), str.end());
	}
protected:
	size_t getMaxLen() const {return MAX_LEN;}
};


class VendorID : public CECBytes
{
public:
	enum {
		MAX_LEN = 3,
	};

	VendorID(uint8_t byte0, uint8_t byte1, uint8_t byte2) : CECBytes (NULL, 0) {
        uint8_t bytes[MAX_LEN];
        bytes[0] = byte0;
        bytes[1] = byte1;
        bytes[2] = byte2;
        str.insert(str.begin(), bytes, bytes + MAX_LEN);
	}

	VendorID(const uint8_t *buf, size_t len) : CECBytes (buf, len > MAX_LEN ? MAX_LEN : len) {
    }

	VendorID(const CECFrame &frame, size_t startPos) : CECBytes (frame, startPos, MAX_LEN) {
    };

protected:
	size_t getMaxLen() const {return MAX_LEN;}

};

class PhysicalAddress : public CECBytes
{
    public:
	enum {
		MAX_LEN = 2,
	};

	PhysicalAddress(uint8_t byte0, uint8_t byte1, uint8_t byte2, uint8_t byte3) : CECBytes (NULL, 0) {
        uint8_t bytes[MAX_LEN];
        bytes[0] = (byte0 & 0x0F)<< 4 | (byte1 & 0x0F);
        bytes[1] = (byte2 & 0x0F)<< 4 | (byte3 & 0x0F);
        str.insert(str.begin(), bytes, bytes + MAX_LEN);
    }

    PhysicalAddress(uint8_t *buf, size_t len = MAX_LEN) : CECBytes(buf, MAX_LEN) {
        Assert(len >= MAX_LEN);
    }

	PhysicalAddress(const CECFrame &frame, size_t startPos) : CECBytes (frame, startPos, MAX_LEN) {
    };

	PhysicalAddress(std::string &addr)         : CECBytes (NULL, 0) {
		uint8_t byte[4];
		uint8_t bytes[MAX_LEN];
		size_t dotposition = 0;
		int i = 0;

		Assert((addr.length() != 0 && addr.length() == 7));
		
		while (addr.length()    && i < 4)
		{
		  byte[i++] = stoi(addr,&dotposition,16);
		  if (addr.length() > 1)
		  {
		   	 addr = addr.substr(dotposition+1);
		  }
		  else
		  {
		 	 break; 	
		  }
		}
		
        bytes[0] = (byte[0] & 0x0F)<< 4 | (byte[1] & 0x0F);
        bytes[1] = (byte[2] & 0x0F)<< 4 | (byte[3] & 0x0F);
        str.insert(str.begin(), bytes, bytes + MAX_LEN);
    }

	uint8_t getByteValue( int index) const {
		uint8_t val = 0;

		Assert(index < 4);

		switch(index)
		{
			case 0: 
			{
				val = (int) ((str[0] & 0xF0) >> 4);
			}
				break;
			case 1: 
			{
				val = (int) (str[0] & 0x0F);
			}
				break;
			
			case 2: 
			{
				val = (int) ((str[1] & 0xF0) >> 4);
			}
				break;

			case 3: 
			{
				val = (int) (str[1] & 0x0F);
			}
				break;
		}

		return val;
    }

    const std::string toString(void) const {
		std::stringstream stream;
        stream << (int)((str[0] & 0xF0) >> 4)<< "." << (int)(str[0] & 0x0F) << "." << (int)((str[1] & 0xF0) >> 4) << "." << (int)(str[1] & 0x0F);
		return stream.str();
    }

    const std::string name(void) const {
        return "PhysicalAddress";
    }

protected:
	size_t getMaxLen() const {return MAX_LEN;}
};

class  LogicalAddress : public CECBytes 
{
public:
    enum {
        MAX_LEN = 1,
    };

    static const LogicalAddress kTv;

    enum {
    	TV 						= 0,
    	RECORDING_DEVICE_1 		= 1,
    	RECORDING_DEVICE_2		= 2,
        TUNER_1 				= 3,
        PLAYBACK_DEVICE_1 		= 4,
        AUDIO_SYSTEM 			= 5,
        TUNER_2 				= 6,
        TUNER_3 				= 7,
        PLAYBACK_DEVICE_2 		= 8,
        RECORDING_DEVICE_3 		= 9,
        TUNER_4 				= 10,
        PLAYBACK_DEVICE_3 		= 11,
        RESERVED_12 			= 12,
        RESERVED_13 			= 13,
        SPECIFIC_USE 			= 14,
        UNREGISTERED 			= 15,
        BROADCAST				= UNREGISTERED,
    };

    LogicalAddress(int addr = UNREGISTERED) : CECBytes((uint8_t)addr) { };

    const std::string toString(void) const
    {
    	static const char *names_[] = {
    	"TV",
    	"Recording Device 1",
    	"Recording Device 2",
    	"Tuner 1",
    	"Playback Device 1",
    	"Audio System",
    	"Tuner 2",
    	"Tuner 3",
    	"Playback Device 2",
    	"Recording Device 3",
    	"Tuner 4",
    	"Playback Device 3",
    	"Reserved 12",
    	"Reserved 13",
    	"Specific Use",
    	"Broadcast/Unregistered",
    	};

	if (validate())
	{
		return names_[str[0]];
	}
	else
	{
		CCEC_LOG(LOG_WARN,"Unknown logical address:%x\n", str[0]);
		return "Unknown";
	}
    }

	int toInt(void) const {
		return str[0];
	}

	bool validate(void) const {
		return ((str[0] <= BROADCAST));
	}

    int getType(void) const {

        if (!validate()) {
            throw InvalidParamException();
        }

        static int _type[] = {
            DeviceType::TV,
            DeviceType::RECORDING_DEVICE,
            DeviceType::RECORDING_DEVICE,
            DeviceType::TUNER,
            DeviceType::PLAYBACK_DEVICE,
            DeviceType::AUDIO_SYSTEM,
            DeviceType::TUNER,
            DeviceType::TUNER,
            DeviceType::PLAYBACK_DEVICE,
            DeviceType::RECORDING_DEVICE,
            DeviceType::TUNER,
            DeviceType::PLAYBACK_DEVICE,
            DeviceType::RESERVED,
            DeviceType::RESERVED,
            DeviceType::RESERVED,
            DeviceType::RESERVED,
        };

        return _type[str[0]];
    }

	LogicalAddress(const CECFrame &frame, size_t startPos) : CECBytes (frame, startPos, MAX_LEN) {
    };
protected:
	size_t getMaxLen() const {return MAX_LEN;}
};

class Version : public CECBytes 
{
public:
    enum {
        MAX_LEN = 1,
    };

    enum {
    	V_RESERVED_0,
    	V_RESERVED_1,
    	V_RESERVED_2,
    	V_RESERVED_3,
    	V_1_3a,
    	V_1_4,
	V_2_0,
    };

	Version(int version) : CECBytes((uint8_t)version) { };

	bool validate(void) const {
		return ((str[0] <= V_2_0) && (str[0] >= V_1_3a));
	}

	const std::string toString(void) const
	{
		static const char *names_[] = {
				"Reserved",
				"Reserved",
				"Reserved",
				"Reserved",
				"Version 1.3a",
				"Version 1.4",
				"Version 2.0"
		};

		if (validate())
		{
			return names_[str[0]];
		}
		else
		{
			CCEC_LOG(LOG_WARN,"Unknown version:%x\n", str[0]);
			return "Unknown";
		}
	}

	Version (const CECFrame &frame, size_t startPos) : CECBytes (frame, startPos, MAX_LEN) {
    };
protected:
	size_t getMaxLen() const {return MAX_LEN;}
};

class PowerStatus : public CECBytes 
{
public:
    enum {
        MAX_LEN = 1,
    };

    enum {
        ON = 0,
        STANDBY = 0x01,
        IN_TRANSITION_STANDBY_TO_ON = 0x02,
        IN_TRANSITION_ON_TO_STANDBY = 0x03,
        POWER_STATUS_NOT_KNOWN = 0x4, 
        POWER_STATUS_FEATURE_ABORT = 0x05,
   };

	PowerStatus(int status) : CECBytes((uint8_t)status) { };

	bool validate(void) const {
		return ((str[0] <= IN_TRANSITION_ON_TO_STANDBY)/* && (str[0] >= ON)*/);
	}

	const std::string toString(void) const
	{
		static const char *names_[] = {
			"On",
			"Standby",
			"In transition Standby to On",
			"In transition On to Standby",
			"Not Known",
			"Feature Abort"
		};

		if (validate())
		{
			return names_[str[0]];
		}
		else
		{
			CCEC_LOG(LOG_WARN,"Unknown powerstatus:%x\n", str[0]);
			return "Unknown";
		}
	}

	int toInt(void) const {
		return str[0];
	}

	PowerStatus (const CECFrame &frame, size_t startPos) : CECBytes (frame, startPos, MAX_LEN) {
	};

protected:
	size_t getMaxLen() const {return MAX_LEN;}
};

class RequestAudioFormat : public CECBytes
{
  public :
    enum {
        MAX_LEN = 1,
    };

	enum {

          SAD_FMT_CODE_LPCM =1 ,		       // 1
 	  SAD_FMT_CODE_AC3,	      // 2
 	  SAD_FMT_CODE_MPEG1,		    //   3
          SAD_FMT_CODE_MP3,	      // 4
          SAD_FMT_CODE_MPEG2,	  //     5
	  SAD_FMT_CODE_AAC_LC,	      // 6
          SAD_FMT_CODE_DTS,	      // 7
          SAD_FMT_CODE_ATRAC,	      // 8
          SAD_FMT_CODE_ONE_BIT_AUDIO,  // 9
          SAD_FMT_CODE_ENHANCED_AC3,	 // 10
          SAD_FMT_CODE_DTS_HD,	 // 11
          SAD_FMT_CODE_MAT,	    //  12
          SAD_FMT_CODE_DST,	    //  13
          SAD_FMT_CODE_WMA_PRO, 	 // 14
          SAD_FMT_CODE_EXTENDED,		//  15
	};
	RequestAudioFormat(uint8_t AudioFormatIdCode) : CECBytes((uint8_t)AudioFormatIdCode) { };
        RequestAudioFormat(const CECFrame &frame, size_t startPos) : CECBytes (frame, startPos,MAX_LEN) { };
	const std::string toString(void) const
        {
		static const char *AudioFormtCode[] = {
				"Reserved",
				"LPCM",
				"AC3",
				"MPEG1",
				"MP3",
				"MPEG2",
				"AAC",
				"DTS",
				"ATRAC",
				"One Bit Audio",
				"E-AC3",
				"DTS-HD",
				"MAT",
				"DST",
				"WMA PRO",
				"Reserved for Audio format 15",
    	};
		/* audio formt code uses 6 bits but codes are defined only for 15 codes */
    	return AudioFormtCode[str[0] & 0xF];
        }
	int getAudioformatId(void) const {

            return (str[0]>>6);
		};

	int getAudioformatCode(void) const {

		return (str[0] & 0x3F);

	};
protected:
	size_t getMaxLen() const {return MAX_LEN;}
};

class ShortAudioDescriptor : public CECBytes
{
  public :
    enum {
        MAX_LEN = 3,
    };

	enum {

          SAD_FMT_CODE_LPCM =1 ,		       // 1
 		  SAD_FMT_CODE_AC3,	      // 2
 		  SAD_FMT_CODE_MPEG1,		    //   3
          SAD_FMT_CODE_MP3,	      // 4
          SAD_FMT_CODE_MPEG2,	  //     5
		  SAD_FMT_CODE_AAC_LC,	      // 6
          SAD_FMT_CODE_DTS,	      // 7
          SAD_FMT_CODE_ATRAC,	      // 8
          SAD_FMT_CODE_ONE_BIT_AUDIO,  // 9
          SAD_FMT_CODE_ENHANCED_AC3,	 // 10
          SAD_FMT_CODE_DTS_HD,	 // 11
          SAD_FMT_CODE_MAT,	    //  12
          SAD_FMT_CODE_DST,	    //  13
          SAD_FMT_CODE_WMA_PRO, 	 // 14
          SAD_FMT_CODE_EXTENDED,		//  15

	};

	ShortAudioDescriptor(uint8_t *buf, size_t len = MAX_LEN) : CECBytes(buf, MAX_LEN) {
               Assert(len >= MAX_LEN);
        };
        ShortAudioDescriptor(const CECFrame &frame, size_t startPos) : CECBytes (frame, startPos,MAX_LEN) {
		   };
	const std::string toString(void) const
        {

		static const char *AudioFormtCode[] = {
				"Reserved",
				"LPCM",
				"AC3",
				"MPEG1",
				"MP3",
				"MPEG2",
				"AAC",
				"DTS",
				"ATRAC",
				"One Bit Audio",
				"E-AC3",
				"DTS-HD",
				"MAT",
				"DST",
				"WMA PRO",
				"Reserved for Audio format 15",
    	        };
   	/* audio formt code uses 6 bits but codes are defined only for 15 codes */
        return AudioFormtCode[(str[0] >> 3) & 0xF];
    }

	uint32_t getAudiodescriptor(void) const	{
         uint32_t audiodescriptor;

		audiodescriptor =  (str[0] | str[1] << 8 | str[2] << 16);
		return audiodescriptor;

	};

	int getAudioformatCode(void) const	{
         uint8_t audioformatCode;
         audioformatCode = ((str[0] >> 3) & 0xF);
		 return audioformatCode;

	};
	uint8_t getAtmosbit(void) const {

        bool atmosSupport = false;
		if ((((str[0] >> 3) & 0xF) >= 9))
		{
                      if((str[2] & 0x3) != 0)
                      {
                         atmosSupport = true;
                      }

		}
            return atmosSupport;
	};

protected:
	size_t getMaxLen() const {return MAX_LEN;}

};
class SystemAudioStatus : public CECBytes
{
public:
    enum {
        MAX_LEN = 1,
    };

    enum {
            OFF = 0x00,
	    ON = 0x01,
         };

	SystemAudioStatus(int status) : CECBytes((uint8_t)status) { };

	bool validate(void) const {
		return ((str[0] <= ON) );
	}

	const std::string toString(void) const
	{
		static const char *names_[] = {
			"Off",
			"On",
		};

		if (validate())
		{
			return names_[str[0]];
		}
		else
		{
			CCEC_LOG(LOG_WARN,"Unknown SystemAudioStatus:%x\n", str[0]);
			return "Unknown";
		}
	}

	int toInt(void) const {
		return str[0];
	}

	SystemAudioStatus (const CECFrame &frame, size_t startPos) : CECBytes (frame, startPos, MAX_LEN) {
	};

protected:
	size_t getMaxLen() const {return MAX_LEN;}
};
class AudioStatus : public CECBytes
{
   public:
   enum {
        MAX_LEN = 1,
   };

   enum {
          AUDIO_MUTE_OFF = 0x00,
          AUDIO_MUTE_ON  = 0x01,
        };
	AudioStatus(uint8_t status) : CECBytes((uint8_t)status) { };

	const std::string toString(void) const
	{
		static const char *names_[] = {
			"Audio Mute Off",
			"Audio Mute On",
		};
		return names_[((str[0] & 0x80) >> 7)];
	}
	int getAudioMuteStatus(void) const {
            return ((str[0] & 0x80) >> 7);
           };
	int getAudioVolume(void) const {
		return (str[0] & 0x7F);
        }
	AudioStatus ( const CECFrame &frame, size_t startPos) : CECBytes (frame, startPos, MAX_LEN) {
	};
protected:
	size_t getMaxLen() const {return MAX_LEN;}
};
class UICommand : public CECBytes
{
public:
    enum {
        MAX_LEN = 1,
    };

    enum {
           UI_COMMAND_VOLUME_UP          = 0x41,
           UI_COMMAND_VOLUME_DOWN        = 0x42,
           UI_COMMAND_MUTE               = 0x43,
           UI_COMMAND_MUTE_FUNCTION      = 0x65,
           UI_COMMAND_RESTORE_FUNCTION   = 0x66,
           UI_COMMAND_POWER_OFF_FUNCTION = 0x6C,
           UI_COMMAND_POWER_ON_FUNCTION  = 0x6D,
	   UI_COMMAND_UP                 = 0x01,
	   UI_COMMAND_DOWN               = 0x02,
	   UI_COMMAND_LEFT               = 0x03,
	   UI_COMMAND_RIGHT              = 0x04,
	   UI_COMMAND_SELECT             = 0x00,
	   UI_COMMAND_HOME               = 0x09,
	   UI_COMMAND_BACK               = 0x0D,
	   UI_COMMAND_NUM_0              = 0x20,
	   UI_COMMAND_NUM_1              = 0x21,
	   UI_COMMAND_NUM_2              = 0x22,
	   UI_COMMAND_NUM_3              = 0x23,
	   UI_COMMAND_NUM_4              = 0x24,
	   UI_COMMAND_NUM_5              = 0x25,
	   UI_COMMAND_NUM_6              = 0x26,
	   UI_COMMAND_NUM_7              = 0x27,
	   UI_COMMAND_NUM_8              = 0x28,
	   UI_COMMAND_NUM_9              = 0x29,
        };

    UICommand(int command) : CECBytes((uint8_t)command) { };

    int toInt(void) const {
        return str[0];
    }

protected:
	size_t getMaxLen() const {return MAX_LEN;}
};
class AllDeviceTypes : public CECBytes
{
public:
    enum {
        MAX_LEN = 1,
    };

    enum {
        CEC_SWITCH = 2,
        AUDIO_SYSTEM,
        PLAYBACK_DEVICE,
        TUNER,
        RECORDING_DEVICE,
        TV,
    };

    AllDeviceTypes(uint8_t types) : CECBytes((uint8_t)types) { };


    AllDeviceTypes( const CECFrame &frame, size_t startPos) : CECBytes (frame, startPos, MAX_LEN) {
    }

    std::vector<std::string> getAllDeviceTypes(void) const {
        std::bitset <8> device_type_bits(str[0]);
        std::vector<std::string> all_device_types_str;
            for(int i = 7; i >= 2; i--){
                if(device_type_bits[i]){
                    switch(i){
                        case TV:
                            all_device_types_str.push_back("TV");
                            break;
                        case RECORDING_DEVICE:
                            all_device_types_str.push_back("Recording Device");
                            break;
                        case TUNER:
                            all_device_types_str.push_back("Tuner");
                            break;
                        case PLAYBACK_DEVICE:
                            all_device_types_str.push_back("Playback Device");
                           break;
                        case AUDIO_SYSTEM:
                            all_device_types_str.push_back("Audio System");
                            break;
                        case CEC_SWITCH:
                            all_device_types_str.push_back("CEC Switch");
                            break;
                        default:
                            break;
                    }
                }
            }
            return all_device_types_str;
    }

    bool isDeviceTypeTV(void) const {
            bool deviceTypeTV = false;
            std::bitset<8> device_type_bits(str[0]);
            if(device_type_bits[TV]){
                    deviceTypeTV = true;
            }
            return deviceTypeTV;
    }
    bool isRecordingDevice(void) const {
            bool recordingDevice = false;
            std::bitset<8> device_type_bits(str[0]);
            if(device_type_bits[RECORDING_DEVICE]){
                    recordingDevice = true;
            }
            return recordingDevice;
    }
    bool isDeviceTypeTuner(void) const {
            bool deviceTypeTuner = false;
            std::bitset<8> device_type_bits(str[0]);
            if(device_type_bits[TUNER]){
                    deviceTypeTuner = true;
            }
            return deviceTypeTuner;
    }
    bool isPlaybackDevice(void) const {
            bool playbackDevice = false;
            std::bitset<8> device_type_bits(str[0]);
            if(device_type_bits[PLAYBACK_DEVICE]){
                    playbackDevice = true;
            }
            return playbackDevice;
    }
    bool isDeviceTypeAudioSystem(void) const {
            bool audioSystem = false;
            std::bitset<8> device_type_bits(str[0]);
            if(device_type_bits[AUDIO_SYSTEM]){
                    audioSystem = true;
            }
            return audioSystem;
    }
    bool isDeviceTypeCECSwitch(void) const {
            bool cecSwitch = false;
            std::bitset<8> device_type_bits(str[0]);
            if(device_type_bits[CEC_SWITCH]){
                    cecSwitch = true;
            }
            return cecSwitch;
    }

protected:
    size_t getMaxLen() const {return MAX_LEN;}
};
class RcProfile : public CECBytes
{
public:
    enum {
        MAX_LEN = 4,
    };

    enum {
        MEDIA_CONTEXT_MENU,
        MEDIA_TOP_MENU,
        CONTENTS_MENU,
        DEVICE_SETUP_MENU,
        DEVICE_ROOT_MENU,
        RC_PROFILE_BIT = 6,
    };

    RcProfile(uint8_t info) : CECBytes((uint8_t)info) { };


    RcProfile( const CECFrame &frame, size_t startPos, size_t len) : CECBytes (frame, startPos, len) {
    };

    std::vector<std::string> getRcProfile(void) const {

            std::vector<std::string> rc_profile_str;
            std::bitset <8> profile_bit(str[0]);
            if(!profile_bit[RC_PROFILE_BIT]){
                rc_profile_str.push_back("RC Profile TV");
                    if((str[0] & 0xE) == 0xE){
                       rc_profile_str.push_back("RC Profile 4");
                    }
                    else if((str[0] & 0xA) == 0xA){
                        rc_profile_str.push_back("RC Profile 3");
                    }
                    else if((str[0] & 0x6) == 0x6){
                        rc_profile_str.push_back("RC Profile 2");
                    }
                    else if((str[0] & 0x2) == 0x2){
                        rc_profile_str.push_back("RC Profile 1");
                    }
                    else{
                        rc_profile_str.push_back("None of the Profiles");
                    }
            }

            else if(profile_bit[RC_PROFILE_BIT]){
                rc_profile_str.push_back("RC Profile Source");
                for(int i = 4; i >= 0; i--){
                    if(profile_bit[i]){
                        switch(i){
                            case DEVICE_ROOT_MENU:
                                rc_profile_str.push_back("Source can handle Device Root Menu - 0x9");
                                break;
                            case DEVICE_SETUP_MENU:
                                rc_profile_str.push_back("Source can handle Device Setup Menu - 0x0A");
                                break;
                            case CONTENTS_MENU:
                                rc_profile_str.push_back("Source can handle Contents Menu - 0x0B");
                                break;
                            case MEDIA_TOP_MENU:
                                rc_profile_str.push_back("Source can handle Media Top Menu - 0x10");
                                break;
                            case MEDIA_CONTEXT_MENU:
                                rc_profile_str.push_back("Source can handle Media Context-Sensitive Menu - 0x11 ");
                                break;
                            default:
                                rc_profile_str.push_back("Source cannot handle any of the UI commands");
                                break;
                        }
                    }
                }
            }

    return rc_profile_str;
    }
    std::vector<uint8_t> getRcProfileVal(void) const {
        std::vector<uint8_t> rcProfileVal;
        size_t i = 0;
        while(i < str.size()) {
                rcProfileVal.push_back(str[i]);
                if((str[i] & 0x80)){
                        i++;
                }
                else{
                        break;
                }
        }
        return rcProfileVal;
    }

    bool isRcProfileTv(void) const {
        bool rcProfileTv = false;
        std::bitset<8> profile_bit(str[0]);
        if(!profile_bit[RC_PROFILE_BIT]){
            rcProfileTv = true;
        }
        return rcProfileTv;
    }

    bool isRcProfileSource(void) const {
        bool rcProfileSource = false;
        std::bitset<8> profile_bit(str[0]);
        if(profile_bit[RC_PROFILE_BIT]) {
            rcProfileSource = true;
        }
        return rcProfileSource;
    }

    bool rootMenuHandling(void) const {
        bool rootMenuHandle = false;
        std::bitset<8> profile_bit(str[0]);
        if(profile_bit[DEVICE_ROOT_MENU]) {
            rootMenuHandle = true;
        }
        return rootMenuHandle;
    }

    bool setupMenuHandling(void) const {
        bool setupMenuHandle = false;
        std::bitset<8> profile_bit(str[0]);
        if(profile_bit[DEVICE_SETUP_MENU]) {
            setupMenuHandle = true;
        }
        return setupMenuHandle;
    }

    bool contentsMenuHandling(void) const {
        bool contentsMenuHandle = false;
        std::bitset<8> profile_bit(str[0]);
        if(profile_bit[CONTENTS_MENU]) {
            contentsMenuHandle = true;
        }
        return contentsMenuHandle;
    }

    bool mediaTopMenuHandling(void) const {
        bool mediaTopMenuHandle = false;
        std::bitset<8> profile_bit(str[0]);
        if(profile_bit[MEDIA_TOP_MENU]) {
            mediaTopMenuHandle = true;
        }
        return mediaTopMenuHandle;
    }

    bool contextSensitiveMenuHandling(void) const {
        bool contextSensitiveMenuHandle = false;
        std::bitset<8> profile_bit(str[0]);
        if(profile_bit[MEDIA_CONTEXT_MENU]) {
            contextSensitiveMenuHandle = true;
        }
        return contextSensitiveMenuHandle;
    }

protected:
        size_t getMaxLen() const {return MAX_LEN;}
};
class DeviceFeatures : public CECBytes
{
public:
    enum {
        MAX_LEN = 4,
    };

    enum {
        RESERVED,
        ARC_RX_SUPPORT,
        SINK_ARC_TX_SUPPORT,
        SET_AUDIO_RATE_SUPPORT,
        CONTROLLED_BY_DECK,
        SET_OSD_STRING_SUPPORT,
        RECORD_TV_SCREEN_SUPPORT,
    };

    DeviceFeatures(uint8_t info) : CECBytes((uint8_t)info) { };


    DeviceFeatures( const CECFrame &frame, size_t startPos, size_t len) : CECBytes (frame, startPos, len) {};

    std::vector<std::string> getDeviceFeatures() const{

            std::vector<std::string> device_features_str;
            std::bitset<8> features_bit(str[0]);

            for(int i = 6; i >= 0; i--){
                if(features_bit[i]){
                    switch(i){
                        case RECORD_TV_SCREEN_SUPPORT:
                            device_features_str.push_back("TV Supports <Record TV Screen>");
                            break;
                        case SET_OSD_STRING_SUPPORT:
                            device_features_str.push_back("TV supports <Set OSD String>");
                            break;
                        case CONTROLLED_BY_DECK:
                            device_features_str.push_back("Supports being controlled by Deck Control");
                            break;
                        case SET_AUDIO_RATE_SUPPORT:
                            device_features_str.push_back("Source supports <Set Audio Rate>");
                           break;
                        case SINK_ARC_TX_SUPPORT:
                            device_features_str.push_back("Sink supports ARC Tx");
                            break;
                        case ARC_RX_SUPPORT:
                            device_features_str.push_back("Source supports ARC Rx");
                            break;
                        case RESERVED:
                            device_features_str.push_back("Reserved");
                            break;
                        default:
                            device_features_str.push_back("Device does not support any of the features");
                            break;
                    }
                }
            }

            return device_features_str;
      }

      std::vector<uint8_t> getDeviceFeaturesVal(void) const {
        std::vector<uint8_t> deviceFeaturesVal;
        size_t i = 0;
        while(i < str.size()) {
                deviceFeaturesVal.push_back(str[i]);
                if((str[i] & 0x80)){
                        i++;
                }
                else{
                        break;
                }
        }
        return deviceFeaturesVal;
    }

    bool tvRecordScreenSupportBit(void) const {
        bool tvRecordScreenSupport = false;
        std::bitset<8> features_bit(str[0]);
        if(features_bit[RECORD_TV_SCREEN_SUPPORT]){
            tvRecordScreenSupport = true;
        }
        return tvRecordScreenSupport;
    }

    bool tVSetOSDStringSupportBit(void) const {
        bool tVSetOSDStringSupport = false;
        std::bitset<8> features_bit(str[0]);
        if(features_bit[SET_OSD_STRING_SUPPORT]){
            tVSetOSDStringSupport = true;
        }
        return tVSetOSDStringSupport;
    }

    bool controlledByDeckSupportBit(void) const {
        bool controlledByDeckSupport = false;
        std::bitset<8> features_bit(str[0]);
        if(features_bit[CONTROLLED_BY_DECK]){
            controlledByDeckSupport = true;
        }
        return controlledByDeckSupport;
    }

    bool setAudioRateSupportBit(void) const {
        bool  setAudioRateSupport = false;
        std::bitset<8> features_bit(str[0]);
        if(features_bit[SET_AUDIO_RATE_SUPPORT]){
            setAudioRateSupport = true;
        }
        return setAudioRateSupport;
    }

    bool arcTxSupportBit(void) const {
        bool  arcTxSupport = false;
        std::bitset<8> features_bit(str[0]);
        if(features_bit[SINK_ARC_TX_SUPPORT]){
            arcTxSupport = true;
        }
        return arcTxSupport;
    }

    bool arcRxSupportBit(void) const {
        bool  arcRxSupport = false;
        std::bitset<8> features_bit(str[0]);
        if(features_bit[ARC_RX_SUPPORT]){
            arcRxSupport = true;
        }
        return arcRxSupport;
    }

protected:
        size_t getMaxLen() const {return MAX_LEN;}
};
class LatencyInfo : public CECBytes
{
public:
    enum {
        MAX_LEN = 3,
    };
    LatencyInfo(uint8_t info) : CECBytes((uint8_t)info) { };


    LatencyInfo ( const CECFrame &frame, size_t startPos, size_t len) : CECBytes (frame, startPos, len) {};

    uint8_t getVideoLatency(void) const {
        return str[0];
    }

    uint8_t getLatencyFlags(void) const {
            return str[1];
    }

    int getAudioOutputDelay(void) const {
            int audio_output_delay = 0xFF;
            if((str[1] == 0x3) & (str.size() == 3)) {
                audio_output_delay = str[2];
            }
             return audio_output_delay;
    }


protected:
        size_t getMaxLen() const {return MAX_LEN;}
};

CCEC_END_NAMESPACE

#endif


/** @} */
/** @} */
