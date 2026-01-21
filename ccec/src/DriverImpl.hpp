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


#ifndef HDMI_CCEC_DRIVER_IMPL_HPP_
#define HDMI_CCEC_DRIVER_IMPL_HPP_

#include <list>
#include <memory>

#include "osal/Mutex.hpp"
#include "osal/EventQueue.hpp"

#include "osal/ConditionVariable.hpp"
#include "ccec/Driver.hpp"
#include "ccec/Header.hpp"

// AIDL includes
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <utils/String16.h>
#include <com/rdk/hal/hdmicec/IHdmiCec.h>
#include <com/rdk/hal/hdmicec/IHdmiCecController.h>
#include <com/rdk/hal/hdmicec/IHdmiCecEventListener.h>
#include <com/rdk/hal/hdmicec/BnHdmiCecEventListener.h>
#include <com/rdk/hal/hdmicec/SendMessageStatus.h>
#include <com/rdk/hal/hdmicec/State.h>

using CCEC_OSAL::EventQueue;
using CCEC_OSAL::Mutex;

CCEC_BEGIN_NAMESPACE

#define HEADER_OFFSET 0
#define OPCODE_OFFSET 1

class IncomingQueue;

class DriverImpl : public Driver
{
public:
	static void DriverReceiveCallback(int handle, void *callbackData, unsigned char *buf, int len);
	static void DriverTransmitCallback(int handle, void *callbackData, int result);
	typedef EventQueue<CECFrame *> IncomingQueue;

	enum {
		CLOSED = 0,
		CLOSING,
		OPENED,
	};
	DriverImpl(void);
	virtual ~DriverImpl();

	virtual void  open(void) noexcept(false);
	virtual void  close(void) noexcept(false);
	virtual void  read(CECFrame &frame) noexcept(false);
	virtual void  write(const CECFrame &frame) noexcept(false);
	virtual void  writeAsync(const CECFrame &frame) noexcept(false);
	virtual void  removeLogicalAddress(const LogicalAddress &source);
	virtual bool  addLogicalAddress   (const LogicalAddress &source);
	virtual int   getLogicalAddress(int devType);
	virtual void  getPhysicalAddress(unsigned int *physicalAddress);
//	virtual const std::list<LogicalAddress> & getLogicalAddresses(void);
	virtual bool isValidLogicalAddress(const LogicalAddress &source) const;
	virtual void poll(const LogicalAddress &from, const LogicalAddress &to) noexcept(false);
	virtual void printFrameDetails(const CECFrame &frame) noexcept(false);

private:
	IncomingQueue & getIncomingQueue(int nativeHandle);
	
	// AIDL service access
	android::sp<com::rdk::hal::hdmicec::IHdmiCec> getAidlService();
	void initAidlService();

	int status;
	int nativeHandle;
	IncomingQueue rQueue;
        mutable Mutex mutex;
	std::list<LogicalAddress> logicalAddresses;
	
	// AIDL members
	android::sp<com::rdk::hal::hdmicec::IHdmiCec> mAidlService;
	android::sp<com::rdk::hal::hdmicec::IHdmiCecController> mAidlController;
	android::sp<com::rdk::hal::hdmicec::IHdmiCecEventListener> mEventListener;
	mutable Mutex mAidlMutex;

	DriverImpl(const DriverImpl &); /* Not allowed */
	DriverImpl & operator = (const DriverImpl &); /* Not allowed */

};

CCEC_END_NAMESPACE

#endif


/** @} */
/** @} */
