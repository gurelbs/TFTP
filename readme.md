# Minimal UDP File Transfer System

A simple UDP-based file transfer system that allows uploading and downloading files between a client and server.

## Features

- UDP-based file transfer
- Support for uploading files (upload)
- Support for downloading files (download)
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

If no port is specified, the default port (6969) will be used.

### Using the Client

To upload a file:
```bash
./client [server_ip] [port] upload [filename]
```

To download a file:
```bash
./client [server_ip] [port] download [filename]
```

## Implementation Details

- Files are broken into 512-byte blocks for transmission
- Each data block is acknowledged by the receiver
- Basic TFTP-like protocol with simplified operations
