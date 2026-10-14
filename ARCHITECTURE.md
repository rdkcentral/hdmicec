# HDMI-CEC Library Architecture

## Overview

The HDMI-CEC library is a C++ implementation for managing HDMI Consumer Electronics Control (CEC) protocol communications on RDK (Reference Design Kit) platforms. This library provides a layered architecture that abstracts hardware-specific CEC driver implementations and offers high-level APIs for CEC message processing.

## High-Level Architecture

The library follows a three-layer architecture:

```
┌─────────────────────────────────────────────────────┐
│          Application Layer                          │
│  (Uses Connection & MessageProcessor APIs)          │
└─────────────────────────────────────────────────────┘
                        ↓
┌─────────────────────────────────────────────────────┐
│          CCEC Layer (Core CEC Logic)                │
│  - Connection Management                            │
│  - Message Encoding/Decoding                        │
│  - Bus Communication                                │
│  - Frame Processing                                 │
└─────────────────────────────────────────────────────┘
                        ↓
┌─────────────────────────────────────────────────────┐
│          OSAL Layer                                 │
│  (OS Abstraction - Threading, Synchronization)      │
└─────────────────────────────────────────────────────┘
                        ↓
┌─────────────────────────────────────────────────────┐
│          Driver Layer                               │
│  (Hardware-specific CEC Driver Interface)           │
└─────────────────────────────────────────────────────┘
```

## Core Components

### 1. OSAL (OS Abstraction Layer)

**Location:** `osal/`

Provides platform-independent synchronization primitives and utilities:

- **Thread**: POSIX thread wrapper for concurrent execution
- **Mutex**: Mutual exclusion for thread-safe operations
- **ConditionVariable**: Thread synchronization mechanism
- **EventQueue**: Thread-safe event queue for asynchronous processing
- **Runnable/Stoppable**: Interfaces for thread lifecycle management

**Key Features:**
- Platform abstraction for portability
- Thread-safe container implementations
- Exception-based error handling

### 2. CCEC (Core CEC Implementation)

**Location:** `ccec/`

The main CEC protocol implementation with several key subsystems:

#### 2.1 Connection Management

**Classes:** `Connection`, `Bus`

- **Connection**: Primary API for applications to access the CEC bus
  - Manages logical addresses (source/destination)
  - Provides send/receive operations for CEC frames
  - Supports both synchronous and asynchronous messaging
  - Implements frame listener registration

- **Bus**: Singleton managing the physical CEC bus
  - Routes frames between connections and the driver
  - Manages frame listeners
  - Handles polling and device discovery
  - Thread-safe operation using OSAL primitives

#### 2.2 Message Processing

**Classes:** `MessageEncoder`, `MessageDecoder`, `MessageProcessor`

- **MessageEncoder**: Converts high-level message objects to raw CEC frames
- **MessageDecoder**: Parses raw CEC frames into message objects
- **MessageProcessor**: Base class with virtual methods for processing specific CEC messages
  - Implements visitor pattern for message handling
  - Applications extend this to handle specific message types
  - Default implementation discards messages (acts as filter)

**Supported Message Types:**
- Active Source / Inactive Source
- Image View On / Text View On
- Standby, Power Status
- Routing Control (Set Stream Path, Routing Change, etc.)
- OSD Name, Vendor Commands
- Device capabilities (CEC Version, Physical Address)

#### 2.3 Frame and Data Management

**Classes:** `CECFrame`, `Header`, `OpCode`, `Operands`

- **CECFrame**: Raw byte buffer representing complete CEC messages
  - Header block (initiator/destination addresses)
  - OpCode block (message type)
  - Operand block (message parameters)

- **Header**: Encapsulates source and destination logical addresses
- **OpCode**: Defines all CEC operation codes
- **Operands**: Container for message-specific parameters

#### 2.4 Driver Interface

**Classes:** `Driver`, `DriverImpl`

- **Driver**: Abstract interface for hardware CEC driver
  - `open()`, `close()`: Resource management
  - `read()`, `write()`: Frame I/O operations
  - `poll()`: Device presence detection
  - Singleton pattern for system-wide access

- **DriverImpl**: Concrete implementation
  - Callbacks for asynchronous receive/transmit
  - Incoming frame queue management
  - State machine for driver lifecycle (CLOSED/OPENING/OPENED/CLOSING)
  - ACK/NACK handling (SENT_AND_ACKD, SENT_FAILED, SENT_BUT_NOT_ACKD)

#### 2.5 Host Integration

**Interface:** `Host.hpp`

C-compatible API for platform-specific host implementations:

- Device status monitoring (power state, connection, OSD name)
- Policy management (TV/STB power control)
- Callback mechanisms for host notifications
- Error code definitions

## Communication Flow

### Sending Messages

```
Application
    ↓ creates Message object
MessageEncoder
    ↓ encode to CECFrame
Connection
    ↓ send() or sendAsync()
Bus
    ↓ queue and route
Driver
    ↓ transmit to hardware
CEC Bus
```

### Receiving Messages

```
CEC Bus
    ↓ hardware interrupt
Driver (DriverReceiveCallback)
    ↓ enqueue CECFrame
Bus
    ↓ notify listeners
FrameListener(s)
    ↓ filter and process
MessageDecoder
    ↓ decode to Message object
MessageProcessor
    ↓ process() method
Application
```

## Design Patterns

1. **Singleton Pattern**: `Bus`, `Driver` - ensures single instance per system
2. **Observer Pattern**: `FrameListener`, `FrameFilter` - event notification
3. **Factory Pattern**: Message creation through encoder/decoder
4. **Strategy Pattern**: `MessageProcessor` - pluggable message handling
5. **Template Pattern**: OSAL abstractions for platform independence

## Thread Safety

- All public APIs are thread-safe using OSAL Mutex
- Bus uses internal locking for listener management
- Driver callbacks execute on separate threads
- EventQueue provides thread-safe message queuing
- Asynchronous operations recommended to avoid blocking

## Build System

- **Autotools-based**: `configure.ac`, `Makefile.am`
- **Dependencies**: glib-2.0 (≥0.10.28)
- **Subdirectories**: cfg, osal, ccec, tests
- **Output**: Shared libraries for OSAL and CCEC components
- **Build scripts**: `build.sh`, `rdk_build.sh` for RDK integration

## Testing

**Location:** `tests/`

- **BasicTest.cpp**: Fundamental API validation
- **CECCmdTest.cpp**: Command processing tests
- **CECMonitor.cpp**: Bus monitoring utility

## Key Design Decisions

1. **Asynchronous-First**: Library prioritizes async operations due to CEC's inherent latency and unreliable device responses
2. **Layered Abstraction**: OSAL enables portability across platforms
3. **Type-Safe Messages**: C++ classes for each message type prevent errors
4. **Extensible Processing**: Virtual methods allow custom message handling
5. **Exception-Based Errors**: Uses exceptions for error propagation in critical paths

## Extension Points

Applications can extend the library by:

1. Implementing custom `MessageProcessor` subclasses
2. Creating custom `FrameListener` implementations
3. Implementing platform-specific `Driver` backends
4. Extending host integration callbacks

## Version

Current version: 1.0.7 (as per CHANGELOG.md)
