#ifndef STUB_LINUX_ANDROID_BINDER_H
#define STUB_LINUX_ANDROID_BINDER_H

#include <cstdint>
#include <sys/ioctl.h>

typedef uintptr_t binder_uintptr_t;

struct binder_version {
  int32_t protocol_version;
};

struct binder_write_read {
  uint64_t write_size;
  uint64_t write_consumed;
  binder_uintptr_t write_buffer;
  uint64_t read_size;
  uint64_t read_consumed;
  binder_uintptr_t read_buffer;
};

struct binder_transaction_data {
  union {
    uint32_t handle;
    binder_uintptr_t ptr;
  } target;
  binder_uintptr_t cookie;
  uint32_t code;
  uint32_t flags;
  int32_t sender_pid;
  int32_t sender_euid;
  uint64_t data_size;
  uint64_t offsets_size;
  union {
    struct {
      binder_uintptr_t buffer;
      binder_uintptr_t offsets;
    } ptr;
    uint8_t buf[8];
  } data;
};

#define BINDER_WRITE_READ _IOWR('b', 1, struct binder_write_read)
#define BC_TRANSACTION 0x0
#define TF_ACCEPT_FDS 0x10
#define BR_REPLY 0x1
#define BR_DEAD_REPLY 0x2
#define BR_FAILED_REPLY 0x3
#define BR_TRANSACTION_COMPLETE 0x4
#define BR_NOOP 0x5
#define BR_OK 0x6

#endif
