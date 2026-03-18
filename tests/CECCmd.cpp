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
* @defgroup tests
* @{
**/


#include <stdio.h>
#include <unistd.h>
#include "libIBus.h"

#include "ccec/Messages.hpp"
#include "ccec/Assert.hpp"
#include "ccec/Connection.hpp"
#include "ccec/host/RDK.hpp"
#include "ccec/MessageEncoder.hpp"
#include "ccec/LibCCEC.hpp"
#include "ccec/CECFrame.hpp"



//The tool is to convert the hex bytes in command line to CECFrame and send it out directly
//CECCmd <hex bytes>
//E.g. CECCmd 3F 82 10 00 /From Tuner To Broadcast, Active_Source

int main(int argc, char *argv[])
{
    int i = 0;

    LibCCEC::getInstance().init();
    sleep(1);

    if (0 != argc && argc > 1)
    {
        printf("Count = %d \n Data : ", argc);
        
        CECFrame frame;
        for(i = 1; i < argc; i++)
        {
            unsigned char byte = (unsigned char)strtol(argv[i], NULL, 16);
            printf("%02x ", byte);
            frame.append(byte);
        }
        printf("\n");

        try {
            // Send frame directly using Connection
            Connection conn(LogicalAddress::UNREGISTERED, true);
            conn.sendTo(LogicalAddress::BROADCAST, frame);
            printf("CEC frame sent successfully\n");
        }
        catch(Exception &e) {
            printf("Failed to send CEC frame\n");
        }
    }
    else
    {
        printf("Usage: CECCmd <hex byte 1> <hex byte 2> ...\n");
    }

    try{
        LibCCEC::getInstance().term();
	}
	catch(Exception &e)
	{
        CCEC_LOG( LOG_EXP, "CEC Mgr:: Caught Exception while calling LibCCEC::term()\r\n");
    }
}
//Note: To enable yocto build for the test app, please add the folder name 'tests' in the 
//SUBDIRS & DIST_SUBDIRS parameters in /hdmicec/Makefile.am


/** @} */
/** @} */
