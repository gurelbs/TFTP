# Minimal UDP File Transfer System

A simple UDP-based file transfer system that allows uploading and downloading files between a client and server.

## Features

- UDP-based file transfer
- Support for uploading / downloading / deleting files
- Basic error handling

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
./server [port]
```

### Using the Client

```bash
./client [server_ip] [port] <upload|download|delete> [filename]
```
for example:

```bash
./client 127.0.0.1 6969 upload readme.md
```
```bash
./client 127.0.0.1 6969 download readme.md
```
```bash
./client 127.0.0.1 6969 delete readme.md
```

## Implementation Details

- Files are broken into 512-byte blocks for transmission
- Each data block is acknowledged by the receiver
- Basic TFTP-like protocol with simplified operations
