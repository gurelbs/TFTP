# Secure UDP File Transfer System

A secure UDP-based file transfer system that allows uploading and downloading files between a client and server with encryption and integrity verification.

## Security Features

- **AES-128 Encryption**: All file data is encrypted using AES-128 to ensure secure communication
- **MD5 Integrity Checking**: Files are verified using MD5 hash to ensure they are transferred correctly

## Additional Features

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

#### use sudo for port 69

```bash
sudo ./server
```
OR 

```bash
./server [port]
```

### Using the Client

```bash
./client [server_ip] [port] <upload|download|delete> [filename]
```
for example:

```bash
./client 127.0.0.1 69 upload readme.md
```
```bash
./client 127.0.0.1 69 download readme.md
```
```bash
./client 127.0.0.1 69 delete readme.md
```

## Implementation Details

- Files are broken into 512-byte blocks for transmission
- Each data block is acknowledged by the receiver
- Basic TFTP-like protocol with simplified operations
- File data is encrypted using AES-128
- MD5 hash is used for integrity verification
