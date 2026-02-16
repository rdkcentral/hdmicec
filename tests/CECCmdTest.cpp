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

#include <iostream>
#include <string>

#include "libIBus.h"
#include "ccec/Messages.hpp"
#include "ccec/Assert.hpp"
#include "ccec/Connection.hpp"
#include "ccec/host/RDK.hpp"
#include "ccec/MessageEncoder.hpp"
#include "ccec/LibCCEC.hpp"
#include "ccec/CECFrame.hpp"

using namespace std;
using namespace CCEC;

int main(int argc, char *argv[])
{
    int i = 0, j = 0;
    string command;
    size_t pos = 0;
    std::string token;
    string delimiter = ":";
    bool running = true;
    bool inited = false;

    Connection *testConnection = NULL;

    cout << "********Entered CECCmd tool***********\n";
    cout << "Options : " << endl;
    cout << "1 - To enable CEC\n2 - To disable CEC\n";
    cout << "3 - Send CEC Command\n4 - Exit test application \n";
    while(running)
    {
        cout << "Please enter your selection : \n";
        if (!(cin >> j)) {
            cout << "Please enter numbers only." << endl;
            cin.clear();
            cin.ignore(10000,'\n');
            continue;
        }
       
        switch (j)
        {
            case 1:
                if(false == inited)
                {
                    cout << "LibCCEC init....................\n";
                    LibCCEC::getInstance().init();
                    inited = true;
                }
            break;
            case 2:
                if(true == inited)
                {
                    cout << "LibCCEC term....................\n";
                    try{
                        LibCCEC::getInstance().term();
	                }
	                catch(Exception &e)
	                {
                        CCEC_LOG( LOG_EXP, "I-ARM CEC Mgr:: Caught Exception while calling LibCCEC::term()\r\n");
                    }
                    inited = false;
                }
            break;
            case 3:
                 cout << "Please enter your CEC Command - eg: 3F:82:10:00" << endl;
                 cin >> command;
                 cout << "Command is : " << command << endl;
                 i = 0;
                 
                 try {
                     CECFrame frame;
                     while ((pos = command.find(delimiter)) != string::npos) {
                         token = command.substr(0, pos);
                         frame.append((unsigned char)strtol(token.c_str(), NULL, 16));
                         command.erase(0, pos + delimiter.length());
                     }
                     frame.append((unsigned char)strtol(command.c_str(), NULL, 16));
                     
                     // Send frame directly using Connection
                     Connection conn(LogicalAddress::UNREGISTERED, true);
                     conn.sendTo(LogicalAddress::BROADCAST, frame);
                     cout << "CEC frame sent successfully" << endl;
                 }
                 catch(Exception &e) {
                     cout << "Failed to send CEC frame" << endl;
                 }
            break;
            case 4:
                if(inited != false)
                {
                    try{
                        LibCCEC::getInstance().term();
	                }
	                catch(Exception &e)
	                {
                        CCEC_LOG( LOG_EXP, \"CEC Mgr:: Caught Exception while calling LibCCEC::term()\\r\\n\");
                    }
                }

                running = false;
            break;
            default:
               cout << "Invalid Entry" << endl;
               j = 0;
            break;
        }
    }
    
    return 0;
}
//Note: To enable yocto build for the test app, please add the folder name 'tests' in the 
//SUBDIRS & DIST_SUBDIRS parameters in /hdmicec/Makefile.am


/** @} */
/** @} */
