/*
 * Copyright 2026 RDK Management
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

/*
 * ServiceManagerCheck.cpp —  Checking the availability of the Android ServiceManager via Binder IPC.
 *
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <iostream>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/android/binder.h>

#include "ServiceManagerCheck.h"
#include "ccec/Util.hpp"

// --- Internal implementation details ---
namespace {

// --- Pure Legacy 32-bit Architecture Layouts ---
#pragma pack(push, 4)
struct binder_write_read_v7 {
    uint32_t write_size;       
    uint32_t write_consumed;   
    uint32_t write_buffer;     
    uint32_t read_size;        
    uint32_t read_consumed;    
    uint32_t read_buffer;      
};

struct binder_transaction_data_v7 {
    union {
        uint32_t handle;
        uint32_t ptr;          
    } target;
    uint32_t cookie;           
    uint32_t code; 
    uint32_t flags;
    int32_t  sender_pid;
    int32_t  sender_euid;
    uint32_t data_size;        
    uint32_t offsets_size;     
    union {
        struct {
            uint32_t buffer;   
            uint32_t offsets;  
        } ptr;
        uint8_t buf[8];
    } data;
};
#pragma pack(pop)

// --- Configuration Constants ---
constexpr uint32_t BINDER_MMAP_SIZE_V7  = (128 * 1024);
constexpr uint32_t BINDER_MMAP_SIZE_V8  = (1024 * 1024);
constexpr uint32_t BR_REPLY_V7          = 0x80247201; 
constexpr uint32_t BR_REPLY_V8          = 0x80287203;
constexpr uint32_t BR_REPLY_V7_ACTUAL   = 0x7206; 
constexpr uint32_t BR_TRANSACTION_COMPLETE_V7 = 0x720c;
constexpr uint32_t BR_OK_V7             = 0x7205;
constexpr uint32_t PING_TRANSACTION     = 0x5F504E47; // '_PNG'

// Macro definitions for internal use
#define BINDER_VERSION       _IOWR('b', 9, struct binder_version)
#define BINDER_WRITE_READ_V7 _IOWR('b', 1, struct binder_write_read_v7)
#define BC_TRANSACTION_V7    _IOW('c', 0, struct binder_transaction_data_v7)

// --- Unified Payload Aggregator Struct ---
struct BinderTransaction {
    std::vector<uint32_t> write_payload;
    std::vector<uint32_t> read_payload;
    unsigned long ioctl_command = 0;
};

// Helper: Generates v7 structural packets
static BinderTransaction prepare_v7_transaction() {
    BinderTransaction tx;
    tx.ioctl_command = BINDER_WRITE_READ_V7;

    binder_transaction_data_v7 txn{};
    txn.target.handle = 0;          
    txn.code = PING_TRANSACTION;    
    txn.flags = 0;                  
    txn.data_size = 0;              
    txn.offsets_size = 0;

    const size_t tx_words = sizeof(txn) / sizeof(uint32_t);
    tx.write_payload.reserve(1 + tx_words);
    tx.write_payload.push_back(BC_TRANSACTION_V7);
    
    const auto* raw_ptr = reinterpret_cast<const uint32_t*>(&txn);
    tx.write_payload.insert(tx.write_payload.end(), raw_ptr, raw_ptr + tx_words);
    tx.read_payload.resize(256, 0);
    return tx;
}

// Helper: Generates v8 (Current System Context) structural packets
static BinderTransaction prepare_v8_transaction() {
    BinderTransaction tx;
    tx.ioctl_command = BINDER_WRITE_READ;

    struct binder_transaction_data txn{};
    std::memset(&txn, 0, sizeof(txn));
    txn.target.handle = 0;          
    txn.code = PING_TRANSACTION;    
    txn.flags = TF_ACCEPT_FDS;      
    txn.data_size = 0;              
    txn.offsets_size = 0;

    const size_t tx_words = sizeof(txn) / sizeof(uint32_t);
    tx.write_payload.reserve(1 + tx_words);
    tx.write_payload.push_back(BC_TRANSACTION);

    const auto* raw_ptr = reinterpret_cast<const uint32_t*>(&txn);
    tx.write_payload.insert(tx.write_payload.end(), raw_ptr, raw_ptr + tx_words);
    tx.read_payload.resize(256, 0);
    return tx;
}

// --- Common Protocol Engine Core ---
static bool execute_binder_ping(const int binder_fd, const int protocol_version) {
    const BinderTransaction tx = (protocol_version == 7) ? prepare_v7_transaction() : prepare_v8_transaction();
    uint32_t bytes_consumed = 0;

    CCEC_LOG(LOG_INFO, "[*] Routing Ping via version %d layout engine...\n", protocol_version);

    if (protocol_version == 7) {
        binder_write_read_v7 bwr{};
        const size_t write_size = tx.write_payload.size() * sizeof(uint32_t);
        const size_t read_size = tx.read_payload.size() * sizeof(uint32_t);
        bwr.write_size = write_size;
        bwr.write_consumed = 0;
        bwr.write_buffer = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tx.write_payload.data()));
        bwr.read_size = read_size;
        bwr.read_consumed = 0;
        bwr.read_buffer = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tx.read_payload.data()));

        const unsigned long ioctl_cmd = tx.ioctl_command;
        if (ioctl(binder_fd, ioctl_cmd, &bwr) < 0) {
            CCEC_LOG(LOG_ERROR, "[-] ioctl execution map allocation failed\n");
            return false;
        }
        bytes_consumed = bwr.read_consumed;
    } else {
        struct binder_write_read bwr{};
        std::memset(&bwr, 0, sizeof(bwr));
        const size_t write_size = tx.write_payload.size() * sizeof(uint32_t);
        const size_t read_size = tx.read_payload.size() * sizeof(uint32_t);
        bwr.write_size = write_size;
        bwr.write_consumed = 0;
        bwr.write_buffer = reinterpret_cast<binder_uintptr_t>(tx.write_payload.data());
        bwr.read_size = read_size;
        bwr.read_consumed = 0;
        bwr.read_buffer = reinterpret_cast<binder_uintptr_t>(tx.read_payload.data());

        const unsigned long ioctl_cmd = tx.ioctl_command;
        if (ioctl(binder_fd, ioctl_cmd, &bwr) < 0) {
            CCEC_LOG(LOG_ERROR, "[-] ioctl execution map allocation failed: %s\n", std::strerror(errno));
            return false;
        }
        bytes_consumed = bwr.read_consumed;
    }

    // --- Unified Protocol Response Token Parsing Loop ---
    CCEC_LOG(LOG_INFO, "[*] Driver returned %u bytes of response telemetry.\n", bytes_consumed);
    
    const uint32_t* const read_start = tx.read_payload.data();
    const uint32_t* const read_end = read_start + (bytes_consumed / sizeof(uint32_t));
    bool service_manager_alive = false;

    for (const uint32_t* read_ptr = read_start; read_ptr < read_end; ++read_ptr) {
        const uint32_t token = *read_ptr;
        CCEC_LOG(LOG_INFO, "[*] Intercepted response token: 0x%x\n", token);

        if (token == BR_REPLY || token == BR_REPLY_V7_ACTUAL || token == BR_REPLY_V7 || token == BR_REPLY_V8) {
            CCEC_LOG(LOG_INFO, "[+] Explicit reply acknowledgement found!\n");
            service_manager_alive = true;
            break; 
        } 
        if (token == BR_DEAD_REPLY || token == BR_FAILED_REPLY) {
            CCEC_LOG(LOG_ERROR, "[-] Driver faulted payload execution target. Status: 0x%x\n", token);
            break;
        } 
        if (token == BR_TRANSACTION_COMPLETE || token == BR_TRANSACTION_COMPLETE_V7) {
            CCEC_LOG(LOG_INFO, "[+] Transaction safely handed off to Binder kernel layer.\n");
            continue;
        } 
        if (token == BR_NOOP || token == BR_OK || token == BR_OK_V7) {
            continue;
        } 
        
        // Safety Fallback for unexpected or structural multi-word response components
        CCEC_LOG(LOG_WARN, "[!] Structural bound reached or unhandled response code. Breaking parsing thread loop.\n");
        break;
    }

    return service_manager_alive;
}

}  // namespace

bool isServiceManagerAvailable() {
    bool service_manager_alive = false;

    const int binder_fd = open("/dev/binder", O_RDWR | O_CLOEXEC);
    if (binder_fd < 0) {
        CCEC_LOG(LOG_ERROR, "[-] Failed to open /dev/binder\n");
        return service_manager_alive;
    }
    CCEC_LOG(LOG_INFO, "[+] Successfully opened /dev/binder\n");

    binder_version version{};
    if (ioctl(binder_fd, BINDER_VERSION, &version) < 0) {
        CCEC_LOG(LOG_ERROR, "[-] Failed to extract device driver protocol revision metadata\n");
        close(binder_fd);
        return service_manager_alive;
    }
    CCEC_LOG(LOG_INFO, "[+] Binder protocol version detected: %d\n", version.protocol_version);

    const size_t binder_map_size = (version.protocol_version == 7) ? BINDER_MMAP_SIZE_V7 : BINDER_MMAP_SIZE_V8;
    void* const mapped_mem = mmap(nullptr, binder_map_size, PROT_READ, MAP_PRIVATE, binder_fd, 0);
    if (mapped_mem == MAP_FAILED) {
        CCEC_LOG(LOG_ERROR, "[-] Shared address space context instantiation failed\n");
        close(binder_fd);
        return service_manager_alive;
    }
    CCEC_LOG(LOG_INFO, "[+] Memory mapped successfully\n");

    const bool ping_result = execute_binder_ping(binder_fd, version.protocol_version);
    service_manager_alive = ping_result;

    munmap(mapped_mem, binder_map_size);
    close(binder_fd);
    return service_manager_alive;
}
