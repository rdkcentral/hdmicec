#ifndef STUB_COM_RDK_HAL_HDMICEC_IHDMICECCONTROLLER_H
#define STUB_COM_RDK_HAL_HDMICEC_IHDMICECCONTROLLER_H

#include <cstdint>
#include <vector>

#include "binder/IServiceManager.h"
#include "com/rdk/hal/hdmicec/SendMessageStatus.h"

namespace com { namespace rdk { namespace hal { namespace hdmicec {
class IHdmiCecController : public android::IBinder {
public:
  virtual ~IHdmiCecController() = default;

  virtual android::binder::Status addLogicalAddresses(const std::vector<int32_t>&, bool* result) {
    if (result) {
      *result = true;
    }
    return android::binder::Status::ok();
  }

  virtual android::binder::Status removeLogicalAddresses(const std::vector<int32_t>&, bool* result) {
    if (result) {
      *result = true;
    }
    return android::binder::Status::ok();
  }

  virtual android::binder::Status sendMessage(const std::vector<uint8_t>&, SendMessageStatus* status) {
    if (status) {
      *status = SendMessageStatus::ACK_STATE_0;
    }
    return android::binder::Status::ok();
  }
};
}}}}

#endif
