# HDMI-CEC Library Product Documentation

## Product Overview

The HDMI-CEC library is a comprehensive C++ software library that enables RDK-based devices to communicate using the HDMI Consumer Electronics Control (CEC) protocol. CEC allows devices connected via HDMI to control one another without the need for multiple remote controls, enabling a unified home entertainment experience.

This library provides both low-level frame manipulation and high-level message-oriented APIs, making it suitable for integrating CEC functionality into set-top boxes, TVs, audio receivers, and other HDMI-connected consumer electronics devices.

## Key Features

### 1. Comprehensive CEC Protocol Support

The library implements the full HDMI-CEC 1.4 specification, supporting all standard CEC messages:

**Power Management:**
- Active Source / Inactive Source
- Image View On / Text View On
- Standby control
- Power status reporting

**Device Discovery & Addressing:**
- Physical address reporting
- Logical address allocation
- Device polling
- OSD name management

**Routing Control:**
- Set Stream Path
- Routing Change
- Routing Information
- Request Active Source

**Audio Control (ARC/eARC):**
- Initiate/Terminate Audio Return Channel (ARC)
- System Audio Mode control
- Audio status reporting
- Volume control commands

**User Interface Commands:**
- Remote control button pass-through (User Control Pressed/Released)
- Menu navigation
- OSD string display

**Device Information:**
- CEC version reporting
- Vendor ID exchange
- Device capabilities query
- Feature support reporting

**Timer & Recording:**
- Timer programming
- Recording control
- Deck control and status

### 2. Dual API Layers

#### High-Level Message API

Applications work with strongly-typed C++ message objects rather than raw bytes:

```cpp
// Send an Active Source message
CECFrame frame = MessageEncoder().encode(
    Header(LogicalAddress(TUNER_1), LogicalAddress(TV)),
    ActiveSource(PhysicalAddress(1, 0, 0, 0))
);
Connection(LogicalAddress(TUNER_1)).send(frame);
```

**Benefits:**
- Type-safe message construction
- Compile-time validation
- Self-documenting code
- Reduced integration errors

#### Low-Level Frame API

Direct access to CEC frame bytes for specialized use cases:
- Custom message implementation
- Protocol debugging
- Bus monitoring
- Performance optimization

### 3. Asynchronous Communication Model

The library is designed for asynchronous operation, acknowledging the real-world challenges of CEC:

- **Non-blocking sends**: Applications don't wait for acknowledgments
- **Event-driven reception**: Listener pattern for incoming messages
- **Configurable timeouts**: Handle unresponsive devices gracefully
- **Queue management**: Automatic frame queuing during bus contention

**Rationale:** CEC devices often have variable response times or may ignore messages entirely. Asynchronous APIs prevent application blocking and enable responsive user experiences.

### 4. Extensible Message Processing

Applications customize behavior by extending the `MessageProcessor` class:

```cpp
class MyDeviceController : public MessageProcessor {
public:
    void process(const ActiveSource &msg, const Header &header) override {
        // Custom handling for Active Source messages
        if (msg.physicalAddress == myAddress) {
            switchToInput();
        }
    }
    
    void process(const Standby &msg, const Header &header) override {
        // Handle standby requests
        enterPowerSaveMode();
    }
};
```

Supports selective message handling - only override methods for messages of interest.

### 5. Multi-Connection Architecture

Multiple logical connections can coexist on the same physical CEC bus:

- **Logical address management**: Each connection represents a CEC device role
- **Message filtering**: Connections receive only relevant frames
- **Independent listeners**: Different components can monitor specific message types
- **Bus arbitration**: Automatic coordination of simultaneous sends

### 6. Thread-Safe Operations

Built on a robust OS abstraction layer (OSAL):

- **Mutex protection**: All public APIs are thread-safe
- **Event queues**: Thread-safe message buffering
- **Condition variables**: Efficient thread synchronization
- **No deadlocks**: Careful lock ordering and timeout mechanisms

### 7. Platform Abstraction

The OSAL layer ensures portability across different RDK platforms:

- **Thread management**: Platform-independent threading
- **Synchronization primitives**: Mutexes, condition variables
- **Event handling**: Asynchronous event delivery
- **Minimal dependencies**: Only requires glib-2.0 (≥0.10.28)

### 8. Bus Monitoring & Debugging

Built-in tools for development and troubleshooting:

- **CECMonitor**: Real-time CEC bus traffic monitoring
- **Frame logging**: Detailed message tracing
- **State introspection**: Query connection and device status
- **Test utilities**: Validation and conformance testing

## Use Cases

### Smart TV Integration
- Automatically switch to active source input
- Control STB/DVD player power and playback
- Display device OSD names in source selection
- Handle remote control commands from connected devices

### Set-Top Box (STB) Implementation
- Signal content availability (Active Source)
- Power on/off TV when STB powers up/down
- Forward user commands to TV
- Support ARC for audio output

### Audio/Video Receiver (AVR)
- System Audio Control implementation
- HDMI switching based on Active Source
- Volume control integration
- ARC audio routing

### Device Testing & Validation
- CEC protocol conformance testing
- Interoperability verification
- Bus analysis and debugging
- Performance benchmarking

## Integration Points

### Application Layer
Applications integrate by:
1. Creating `Connection` objects for each logical device role
2. Extending `MessageProcessor` for custom message handling
3. Registering `FrameListener` objects for event notifications
4. Calling `send()` or `sendAsync()` to transmit messages

### Platform/Hardware Layer
Platform vendors provide:
1. Hardware driver implementation conforming to `Driver` interface
2. Host module implementation for device-specific behavior
3. Physical address configuration
4. Device type and capabilities definition

### Build Integration
- Autotools-based build system
- pkg-config support for dependency management
- Separate libraries for OSAL and CCEC layers
- Header installation for downstream projects

## Configuration

The library supports runtime configuration through the host interface:

- **Device policies**: Control automatic power-off behavior
- **Logical addresses**: Configure device roles and addresses
- **Physical addresses**: Set HDMI topology position
- **Feature flags**: Enable/disable specific CEC capabilities

## Benefits

1. **Accelerated Development**: Pre-built CEC stack reduces time-to-market
2. **Compliance**: Implements HDMI-CEC specification correctly
3. **Reliability**: Handles edge cases and error conditions
4. **Maintainability**: Clean API separation and extensive documentation
5. **Flexibility**: Supports both simple and advanced use cases
6. **Testability**: Built-in debugging and monitoring tools
7. **Portability**: Runs across RDK platforms with minimal changes

## Example Applications

The library includes practical examples:

- **BasicTest**: Demonstrates fundamental API usage
- **CECMonitor**: Real-time bus monitoring and message decoding
- **CECCmdTest**: Interactive command-line CEC message sender

These serve as both learning tools and starting points for custom applications.

## Limitations & Considerations

- **Bus timing**: CEC has inherent latency; design for asynchronous operation
- **Device compatibility**: Not all devices fully implement CEC specification
- **Single bus**: Typically one CEC bus per HDMI infrastructure
- **No guarantees**: CEC messages may be ignored or NACK'd by receivers
- **Platform-specific**: Requires platform driver implementation

## Version & Support

**Current Version:** 1.0.7  
**License:** Apache License 2.0  
**Maintained By:** RDK Management  
**Repository:** github.com/rdkcentral/hdmicec

For contributions, bug reports, and feature requests, see CONTRIBUTING.md.
