#ifndef STUB_COM_RDK_HAL_HDMICEC_IHDMICECEVENTLISTENER_H
#define STUB_COM_RDK_HAL_HDMICEC_IHDMICECEVENTLISTENER_H

#include <vector>

#include "binder/IServiceManager.h"
#include "com/rdk/hal/hdmicec/SendMessageStatus.h"
#include "com/rdk/hal/hdmicec/State.h"

namespace com { namespace rdk { namespace hal { namespace hdmicec {
class IHdmiCecEventListener : public android::IBinder {
public:
  virtual ~IHdmiCecEventListener() = default;
  virtual android::binder::Status onMessageReceived(const std::vector<uint8_t>&) { return android::binder::Status::ok(); }
  virtual android::binder::Status onStateChanged(State, State) { return android::binder::Status::ok(); }
  virtual android::binder::Status onMessageSent(const std::vector<uint8_t>&, SendMessageStatus) { return android::binder::Status::ok(); }
};
}}}}

#endif
