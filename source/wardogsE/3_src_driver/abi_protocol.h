#pragma once

// Include ntifs.h (kernel) or windows.h (x64 client) before this header.
#define ABI_IOCTL_READ_MEMORY       0x222004
#define ABI_IOCTL_GET_BASE          0x222008
#define ABI_IOCTL_UNLOAD            0x22200C
#define ABI_IOCTL_WRITE_MEMORY      0x222010
#define ABI_IOCTL_QUERY_STATUS      0x222014
#define ABI_IOCTL_GET_PROCESS_INFO  0x222018
#define ABI_IOCTL_READ_BATCH        0x22201C

#define ABI_PROTOCOL_VERSION 1
#define ABI_MAX_TRANSFER 0x10000
#define ABI_MAX_BATCH_COUNT 96
#define ABI_CAP_QUERY_STATUS 0x1
#define ABI_CAP_PROCESS_INFO 0x2
#define ABI_CAP_READ_BATCH   0x4

#pragma pack(push, 1)
typedef struct _ABI_COMMAND {
    ULONG     magic_key;
    ULONG     process_id;
    ULONG_PTR address;
    SIZE_T    size;
} ABI_COMMAND, *PABI_COMMAND;

typedef struct _ABI_STATUS {
    ULONG protocol_version;
    ULONG device_ready;
    ULONG max_transfer;
    ULONG max_batch_count;
    ULONG capabilities;
    ULONG reserved;
} ABI_STATUS, *PABI_STATUS;

typedef struct _ABI_PROCESS_INFO {
    ULONG_PTR image_base;
    ULONG_PTR peb;
} ABI_PROCESS_INFO, *PABI_PROCESS_INFO;

// Followed by count ABI_READ_SEGMENT records. command.size is the output size;
// command.address and reserved are unused. Output gaps are zeroed.
typedef struct _ABI_BATCH_COMMAND {
    ABI_COMMAND command;
    ULONG count;
    ULONG reserved;
} ABI_BATCH_COMMAND, *PABI_BATCH_COMMAND;

typedef struct _ABI_READ_SEGMENT {
    ULONG_PTR address;
    ULONG size;
    ULONG output_offset;
} ABI_READ_SEGMENT, *PABI_READ_SEGMENT;
#pragma pack(pop)

C_ASSERT(sizeof(ABI_COMMAND) == 24);
C_ASSERT(sizeof(ABI_STATUS) == 24);
C_ASSERT(sizeof(ABI_PROCESS_INFO) == 16);
C_ASSERT(sizeof(ABI_BATCH_COMMAND) == 32);
C_ASSERT(sizeof(ABI_READ_SEGMENT) == 16);
