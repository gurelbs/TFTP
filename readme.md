# Secure UDP File Transfer System

A secure UDP-based file transfer system that allows uploading and downloading files between a client and server with encryption and integrity verification.

## Table of Contents

- [Features](#features)
- [Dependencies](#dependencies)
- [Building](#building)
- [Usage](#usage)
- [Implementation Details](#implementation-details)

## Features

### Security Features

- **AES-128 Encryption**: All file data is encrypted using AES-128 to ensure secure communication
- **MD5 Integrity Checking**: Files are verified using MD5 hash to ensure they are transferred correctly

### Additional Features

- UDP-based file transfer
- Support for uploading / downloading / deleting files
- Robust error handling and retry mechanisms
- Verification of file integrity

## Dependencies

- OpenSSL for encryption and hashing

## Building

To build the project, simply run:

```bash
make
```

This will create two executable files:
- `server`: The server program
- `client`: The client program

## Usage

### Starting the Server

```bash
sudo ./server    # Use sudo for default port 69
```

OR 

```bash
./server [port]  # Specify custom port
```

Note: Using port numbers below 1024 (like the default 69) requires root privileges.

### Using the Client

General syntax:
```bash
./client [server_ip] [port] <upload|download|delete> [filename]
```

Examples:

```bash
./client 127.0.0.1 69 upload readme.md
./client 127.0.0.1 69 download readme.md
./client 127.0.0.1 69 delete readme.md
```

Using default settings (localhost:69):
```bash
./client upload example.txt
```

## Implementation Details

- Files are broken into 512-byte blocks for transmission
- Each data block is acknowledged by the receiver
- Basic TFTP-like protocol with simplified operations
- File data is encrypted using AES-128
- MD5 hash is used for integrity verification

## Security Considerations

- This implementation uses static encryption mechanisms for demonstration
- For production use, consider implementing stronger authentication and key exchange
- MD5 is used for simplicity but has known vulnerabilities

## New Module Structure

The codebase has been refactored to separate client and server code into distinct modules for better organization. Common functionality, such as encryption and MD5 calculation, remains abstracted into `common.c` and `common.h`.

### New Files

- `client_operations.c` and `client_operations.h`: Contains client-specific functionality.
- `server_operations.c` and `server_operations.h`: Contains server-specific functionality.

### Updated Makefile

The `Makefile` has been updated to support the new module structure and includes more granular build targets and dependencies.

### Configuration

Hardcoded values and constants have been refactored into configuration files or environment variables for better flexibility.

### Error Handling and Logging

Error handling and logging have been improved for better debugging and user feedback.
