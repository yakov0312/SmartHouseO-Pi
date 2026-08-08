# Protocol Specification v1.0

## Overview

This document describes the binary communication protocol used between clients and the ProtocolManager. The protocol supports both request-response commands and persistent bidirectional streams with configurable flow control.

## Connection Lifecycle

1. **TCP Establishment**: Client connects via TCP
2. **Configuration (Optional)**: Client may negotiate buffer sizes via CONFIG packet
3. **Authentication**: Client sends AUTH command to authenticate
4. **Operation**: Client sends commands or enters stream mode
5. **Termination**: Either party closes the TCP connection

## Packet Structure

All packets follow a common structure: **Header + Payload**

### PacketHeader (Common Header)

Every packet begins with a header containing:

| Field   | Type     | Size    | Description                                        |
| ------- | -------- | ------- | -------------------------------------------------- |
| control | uint8_t  | 1 byte  | Packet type (COMMAND, AUTH, STREAM, CONFIG, Error) |
| len     | uint16_t | 2 bytes | Payload length in bytes (big-endian)               |

Total header size: **3 bytes** (minimum)

### Control Types

| Value | Name    | Description              |
| ----- | ------- | ------------------------ |
| 0     | COMMAND | Request-response command |
| 1     | AUTH    | Authentication command   |
| 2     | STREAM  | Stream data chunk        |
| 3     | CONFIG  | Buffer size negotiation  |
| 4     | Error   | Error response           |

## Packet Types

### 1. CommandPacket (COMMAND, AUTH)

Used for request-response operations. Sent from client to server.

**Structure:**

```text
[PacketHeader: 3 bytes]
[Payload: len bytes]
```

**Payload Format (UTF-8 text):**

```text
[Service] [Action] [Args...]
```

* **Service**: Module name (e.g., "WakeOnLan", "Cloud")
* **Action**: Command to execute (e.g., "Wake", "Upload", "Download")
* **Args**: Optional space-separated arguments

**Examples:**

```text
Login username password
WakeOnLan Wake <device>
Cloud download report.pdf
```

**When Sent:**

* Client wants to execute a command on a specific module
* AUTH control type used specifically for authentication commands

**Server Response:**

Server responds with a CommandPacket containing the result text in the payload.

---

### 2. StreamPacket (STREAM)

Used for continuous binary data transfer after entering stream mode.

**Structure:**

```text
[PacketHeader: 3 bytes]
[Payload: len bytes (binary data)]
```

**When Sent:**

* **Client → Server**: After entering stream mode, sends continuous data chunks
* **Server → Client**: Module-generated async events forwarded via streamEventHandler

**Flow Control:**

The server limits queued stream chunks per connection (`MaxQueuedStreamChunks`). If the limit is reached, the server stops processing new stream packets until the queue drains.
Packets exceeding the configured maximum buffer size are dropped.

**Stream Lifecycle:**

1. Client sends command that returns `createStream=true`
2. Server responds with stream initialization (StreamPacket + ConfigurationPacket)
3. Both parties can now send StreamPackets
4. Either party can terminate by sending empty StreamPacket (len=0) or Error packet

---

### 3. ConfigurationPacket (CONFIG)

Used to negotiate per-connection buffer limits.

**Structure:**

```text
[PacketHeader: 3 bytes]  // control=CONFIG, len=0 (payload in fixed fields)
[maxInputChunk: uint32_t]   // Client's requested max input chunk
[maxOutputChunk: uint32_t]  // Client's requested max output chunk
```

**When Sent:**

* **Client → Server**: After connection, before heavy traffic
* **Server → Client**: Response with accepted values (may be lower than requested)

**Behavior:**

Server clamps requested values to system limits (`MaxInputBuffer`, `MaxOutputBuffer`). Minimum input size is enforced to fit a ConfigurationPacket. The negotiated values apply to all subsequent packets on this connection.

---

### 4. Error Responses

Server sends packets with `control=Error` to indicate failures.

**Structure:**

```text
[PacketHeader: 3 bytes]  // control=Error
[Error message: len bytes (UTF-8)]
```

**When Sent:**

* Unknown service requested
* Malformed command
* Stream processing error
* Authentication failure

## Protocol Flows

### Authentication Flow

```text
Client                                    Server
  |                                          |
  |--- CommandPacket (AUTH) ---------------->|
  |    "Login user pass"                     |
  |                                          |
  |<-- CommandPacket (AUTH) -----------------|
  |    "OK" or "Invalid credentials"         |
  |                                          |
```

### Command Execution Flow

```text
Client                                    Server
  |                                          |
  |--- CommandPacket (COMMAND) ------------->|
  |    "WakeOnLan Wake <device>"             |
  |                                          |
  |    [Queued in command thread pool]       |
  |                                          |
  |<-- CommandPacket (COMMAND) --------------|
  |    "Sent WoL packet                      |
  |                                          |
```

### Stream Establishment Flow

```text
Client                                    Server
  |                                          |
  |--- CommandPacket (COMMAND) ------------->|
  |    "Cloud upload large.bin"              |
  |                                          |
  |<-- StreamPacket -------------------------|
  |    [Initialization data]                 |
  |<-- ConfigurationPacket ------------------|
  |    [Negotiated buffer sizes]             |
  |                                          |
  |=== Bidirectional Stream =================|
  |                                          |
  |--- StreamPacket ------------------------>|
  |    [Binary chunk 1]                      |
  |--- StreamPacket ------------------------>|
  |    [Binary chunk 2]                      |                      
  |                                          |
  |--- StreamPacket (len=0) ---------------->|
  |    [Client signals EOF]                  |
  |                                          |
  |<-- CommandPacket (Error/empty) ----------|
  |    [Stream terminated]                   |
```

## Buffer Management

### Input Buffering

Each connection maintains an `inputBuffer` for incomplete packets. The protocol supports partial packet arrival - data remains buffered until the complete packet arrives.

### Output Buffering

Responses are queued in `outputBuffer` and flushed asynchronously via epoll notifications (`notifyEpoll`).

### Flow Control

**Server-side limits:**

* `MaxInputBuffer`: Maximum input buffer size per connection
* `MaxOutputBuffer`: Maximum output buffer size per connection
* `MaxQueuedStreamChunks`: Maximum stream tasks queued per connection
* `MaxQueuedStreamEvents`: Maximum async events in stream channel

**Backpressure:**

Server returns 0 from `getAvailableInputSpace()` when input buffer is full. Client should pause sending until space becomes available.

## Error Handling

### Protocol Errors

| Error Condition                   | Server Action                              |
| --------------------------------- | ------------------------------------------ |
| Invalid control type              | Disconnect client (`keepAlive=false`)      |
| Oversized packet (>maxInputChunk) | Discard packet, clear buffer               |
| Malformed command                 | Skip packet, log error                     |
| Unknown service                   | Return Error packet with "Unknown service" |
| Stream to non-stream service      | Return Error packet, terminate stream      |

### Connection Errors

* TCP disconnect: `removeUser()` cleans up resources and terminates active streams
* Epoll errors: Socket is closed and connection removed

## Threading Model

* **Epoll Thread**: Calls `process()` for incoming data (non-blocking)
* **Command Threads**: Execute commands from `processCommand()`
* **Stream Threads**: Handle stream chunks from `processStream()`
* **StreamEvent Thread**: Forwards async module events to clients

## Configuration Parameters

Protocol behavior is controlled via configuration file:

| Section  | Key                   | Default | Description                          |
| -------- | --------------------- | ------- | ------------------------------------ |
| Command  | CommandThreads        | 2       | Worker threads for command execution |
| Stream   | StreamThreads         | 4       | Worker threads for stream processing |
| Stream   | MaxQueuedStreamChunks | 4       | Flow control limit per connection    |
| Stream   | MaxQueuedStreamEvents | 16      | Stream channel queue depth           |
| Settings | MaxInputBuffer        | 16384   | Maximum input buffer per connection  |
| Settings | MaxOutputBuffer       | 16384   | Maximum output buffer per connection |

## Implementation Notes

* All multi-byte integers are in **native endianness** (host byte order)
* Strings are **UTF-8 encoded** without null terminators
* The `connectionId` in epoll events correlates to the socket lifetime, not the authenticated user
* Stream mode persists until explicitly terminated or connection closes
* Re-authentication is not required once `auth.authenticated` is true
