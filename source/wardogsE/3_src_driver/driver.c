// ABI non-PnP control driver - standard OS load and DriverUnload lifecycle.
// Client contract: open \\.\AbiComm or the existing \Device\AbiComm0,
// METHOD_BUFFERED IOCTLs, ABI_COMMAND {key, pid, addr, size}.
// Session key: PrefetchParameters\BootId ^ 0x8F2A9C4B, xorshift32.

#include <ntifs.h>
#include <wdmsec.h>
#include "abi_protocol.h"

// ntoskrnl exports absent from wdm.h — declare locally.
NTKERNELAPI
NTSTATUS
MmCopyVirtualMemory(
    PEPROCESS SourceProcess,
    PVOID SourceAddress,
    PEPROCESS TargetProcess,
    PVOID TargetAddress,
    SIZE_T BufferSize,
    KPROCESSOR_MODE PreviousMode,
    PSIZE_T ReturnSize
);

NTKERNELAPI
PPEB
NTAPI
PsGetProcessPeb(
    PEPROCESS Process
);

#define KEY_SALT 0x8F2A9C4BUL
#define ABI_DEVICE_NAME L"\\Device\\AbiComm0"
#define ABI_DOS_NAME L"\\DosDevices\\Global\\AbiComm"

#define MAX_TRANSFER ABI_MAX_TRANSFER

// Process rights from winnt.h; ntifs.h does not expose these three constants.
#define ABI_PROCESS_VM_OPERATION 0x0008
#define ABI_PROCESS_VM_READ      0x0010
#define ABI_PROCESS_VM_WRITE     0x0020

// Device access is public; each memory operation checks the caller's process rights.
#define ABI_SDDL_DEVOBJ_SYS_ALL_AIX_RWX_WORLD_RX_RES \
    L"D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)(A;;GRGWGX;;;WD)(A;;GRGWGX;;;RC)"

static ULONG  g_SessionKey = 0;
static volatile LONG g_DeviceCreated = FALSE;
static EX_RUNDOWN_REF g_RequestRundown;
static BOOLEAN g_LinkCreated = FALSE;
static HANDLE g_StateKey = NULL;
static BOOLEAN g_StateKeyCreated = FALSE;
static BOOLEAN g_KeyPublished = FALSE;
static const GUID g_DeviceClass =
    { 0x162acc5d, 0x6bd7, 0x4386, { 0xaa, 0xe0, 0x35, 0x41, 0x07, 0xfd, 0x19, 0x1b } };

// ---------------------------------------------------------------- session key

static ULONG Xorshift(ULONG key) {
    key ^= key << 13;
    key ^= key >> 17;
    key ^= key << 5;
    return key;
}

static NTSTATUS ReadBootId(PULONG bootId) {
    UNICODE_STRING keyPath, valueName;
    OBJECT_ATTRIBUTES attributes;
    HANDLE keyHandle = NULL;
    ULONG result = 0;
    NTSTATUS status;

    *bootId = 0;
    RtlInitUnicodeString(&keyPath,
        L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\Session Manager"
        L"\\Memory Management\\PrefetchParameters");
    RtlInitUnicodeString(&valueName, L"BootId");
    InitializeObjectAttributes(&attributes, &keyPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = ZwOpenKey(&keyHandle, KEY_READ, &attributes);
    if (!NT_SUCCESS(status))
        return status;

    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)] = { 0 };
    ULONG length = 0;
    status = ZwQueryValueKey(keyHandle, &valueName, KeyValuePartialInformation,
        buffer, sizeof(buffer), &length);
    if (NT_SUCCESS(status)) {
        PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
        if (info->Type == REG_DWORD && info->DataLength == sizeof(ULONG))
            result = *(ULONG*)info->Data;
        else
            status = STATUS_OBJECT_TYPE_MISMATCH;
    }

    ZwClose(keyHandle);
    *bootId = result;
    return status;
}

static ULONG ComputeSessionKey(VOID) {
    ULONG bootId = 0;
    if (!NT_SUCCESS(ReadBootId(&bootId)) || bootId == 0) {
        LARGE_INTEGER systemTime;
        KeQuerySystemTime(&systemTime);
        bootId = systemTime.LowPart ^ systemTime.HighPart;
    }

    ULONG key = Xorshift(bootId ^ KEY_SALT);
    return (key == 0) ? 0xA5A5A5A5UL : key;
}

static NTSTATUS AbiPublishSessionKey(VOID) {
    UNICODE_STRING path, name;
    OBJECT_ATTRIBUTES attributes;
    ULONG disposition = 0;
    HANDLE key = NULL;
    RtlInitUnicodeString(&path, L"\\Registry\\Machine\\SYSTEM\\AbiComm");
    InitializeObjectAttributes(&attributes, &path,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    NTSTATUS status = ZwCreateKey(&key, KEY_SET_VALUE | DELETE,
        &attributes, 0, NULL, REG_OPTION_VOLATILE, &disposition);
    if (!NT_SUCCESS(status))
        return status;

    g_StateKey = key;
    g_StateKeyCreated = disposition == REG_CREATED_NEW_KEY;
    RtlInitUnicodeString(&name, L"SessionKey");
    status = ZwSetValueKey(g_StateKey, &name, 0, REG_DWORD,
        &g_SessionKey, sizeof(g_SessionKey));
    g_KeyPublished = NT_SUCCESS(status);
    return status;
}

static VOID AbiDeleteSessionKey(VOID) {
    if (g_StateKey) {
        if (g_KeyPublished) {
            UNICODE_STRING name;
            RtlInitUnicodeString(&name, L"SessionKey");
            ZwDeleteValueKey(g_StateKey, &name);
        }
        if (g_StateKeyCreated)
            ZwDeleteKey(g_StateKey);
        ZwClose(g_StateKey);
        g_StateKey = NULL;
    }
    g_KeyPublished = FALSE;
    g_StateKeyCreated = FALSE;
}

// ---------------------------------------------------------------- memory ops

static BOOLEAN IsUserRange(ULONG_PTR address, SIZE_T size) {
    ULONG_PTR limit = (ULONG_PTR)MmHighestUserAddress;
    return size != 0 && address <= limit && size - 1 <= limit - address;
}

static NTSTATUS ReferenceTargetProcess(ULONG processId, ACCESS_MASK access,
    PEPROCESS* process) {
    NTSTATUS status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)processId, process);
    if (!NT_SUCCESS(status))
        return status;

    HANDLE handle = NULL;
    status = ObOpenObjectByPointer(*process, OBJ_KERNEL_HANDLE | OBJ_FORCE_ACCESS_CHECK,
        NULL, access, *PsProcessType, UserMode, &handle);
    if (NT_SUCCESS(status)) {
        // Object callbacks can reduce the rights granted to an otherwise valid handle.
        PUBLIC_OBJECT_BASIC_INFORMATION info;
        status = ZwQueryObject(handle, ObjectBasicInformation, &info, sizeof(info), NULL);
        if (NT_SUCCESS(status) && (info.GrantedAccess & access) != access)
            status = STATUS_ACCESS_DENIED;
        ZwClose(handle);
    }
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(*process);
        *process = NULL;
    }
    return status;
}

static NTSTATUS CopyCrossProcess(PEPROCESS sourceProcess, ULONG_PTR sourceAddress,
    PEPROCESS targetProcess, ULONG_PTR targetAddress, SIZE_T size, PSIZE_T transferred) {
    return MmCopyVirtualMemory(sourceProcess, (PVOID)sourceAddress,
        targetProcess, (PVOID)targetAddress, size, KernelMode, transferred);
}

static NTSTATUS ReadTargetMemory(ULONG processId, ULONG_PTR address,
    PVOID kernelBuffer, SIZE_T size) {
    if (!IsUserRange(address, size))
        return STATUS_ACCESS_VIOLATION;
    PEPROCESS process = NULL;
    NTSTATUS status = ReferenceTargetProcess(processId, ABI_PROCESS_VM_READ, &process);
    if (!NT_SUCCESS(status))
        return status;

    SIZE_T transferred = 0;
    status = CopyCrossProcess(process, address, PsGetCurrentProcess(),
        (ULONG_PTR)kernelBuffer, size, &transferred);
    ObDereferenceObject(process);

    if (NT_SUCCESS(status) && transferred != size)
        return STATUS_PARTIAL_COPY;
    return status;
}

static NTSTATUS WriteTargetMemory(ULONG processId, ULONG_PTR address,
    PVOID kernelBuffer, SIZE_T size) {
    if (!IsUserRange(address, size))
        return STATUS_ACCESS_VIOLATION;
    PEPROCESS process = NULL;
    NTSTATUS status = ReferenceTargetProcess(processId,
        ABI_PROCESS_VM_WRITE | ABI_PROCESS_VM_OPERATION, &process);
    if (!NT_SUCCESS(status))
        return status;

    SIZE_T transferred = 0;
    status = CopyCrossProcess(PsGetCurrentProcess(), (ULONG_PTR)kernelBuffer,
        process, address, size, &transferred);
    ObDereferenceObject(process);

    if (NT_SUCCESS(status) && transferred != size)
        return STATUS_PARTIAL_COPY;
    return status;
}

static NTSTATUS GetTargetProcessInfo(ULONG processId, PABI_PROCESS_INFO info) {
    RtlZeroMemory(info, sizeof(*info));

    PEPROCESS process = NULL;
    NTSTATUS status = ReferenceTargetProcess(processId, ABI_PROCESS_VM_READ, &process);
    if (!NT_SUCCESS(status))
        return status;

    PPEB peb = PsGetProcessPeb(process);
    if (!peb || !IsUserRange((ULONG_PTR)peb, 0x10 + sizeof(ULONG_PTR))) {
        ObDereferenceObject(process);
        return STATUS_UNSUCCESSFUL;
    }

    SIZE_T transferred = 0;
    ULONG_PTR imageBase = 0;
    status = CopyCrossProcess(process, (ULONG_PTR)peb + 0x10,
        PsGetCurrentProcess(), (ULONG_PTR)&imageBase, sizeof(imageBase), &transferred);
    ObDereferenceObject(process);

    if (NT_SUCCESS(status) && transferred != sizeof(ULONG_PTR))
        return STATUS_PARTIAL_COPY;
    if (NT_SUCCESS(status)) {
        info->image_base = imageBase;
        info->peb = (ULONG_PTR)peb;
    }
    return status;
}

static NTSTATUS GetTargetImageBase(ULONG processId, PULONG_PTR imageBase) {
    ABI_PROCESS_INFO info;
    NTSTATUS status = GetTargetProcessInfo(processId, &info);
    *imageBase = info.image_base;
    return status;
}

static NTSTATUS ReadTargetMemoryBatch(PVOID systemBuffer, ULONG inputLength,
    ULONG outputLength, PULONG information) {
    *information = 0;
    if (inputLength < sizeof(ABI_BATCH_COMMAND))
        return STATUS_INVALID_PARAMETER;

    ABI_BATCH_COMMAND request = *(PABI_BATCH_COMMAND)systemBuffer;
    if (request.count == 0 || request.count > ABI_MAX_BATCH_COUNT ||
        request.command.size == 0 || request.command.size > MAX_TRANSFER ||
        request.command.size > outputLength ||
        inputLength < sizeof(request) + request.count * sizeof(ABI_READ_SEGMENT))
        return STATUS_INVALID_PARAMETER;

    // METHOD_BUFFERED shares input/output storage. Snapshot all descriptors
    // before clearing or writing the output, including descriptors it overlaps.
    ABI_READ_SEGMENT segments[ABI_MAX_BATCH_COUNT];
    RtlCopyMemory(segments, (PUCHAR)systemBuffer + sizeof(request),
        request.count * sizeof(segments[0]));
    for (ULONG i = 0; i < request.count; ++i) {
        if (!IsUserRange(segments[i].address, segments[i].size))
            return STATUS_ACCESS_VIOLATION;
        if (segments[i].output_offset > request.command.size ||
            segments[i].size > request.command.size - segments[i].output_offset)
            return STATUS_INVALID_PARAMETER;
    }

    PEPROCESS process = NULL;
    NTSTATUS status = ReferenceTargetProcess(request.command.process_id,
        ABI_PROCESS_VM_READ, &process);
    if (!NT_SUCCESS(status))
        return status;

    RtlZeroMemory(systemBuffer, (SIZE_T)request.command.size);
    for (ULONG i = 0; i < request.count; ++i) {
        SIZE_T transferred = 0;
        status = CopyCrossProcess(process, segments[i].address,
            PsGetCurrentProcess(), (ULONG_PTR)systemBuffer + segments[i].output_offset,
            segments[i].size, &transferred);
        if (NT_SUCCESS(status) && transferred != segments[i].size)
            status = STATUS_PARTIAL_COPY;
        if (!NT_SUCCESS(status))
            break;
    }
    ObDereferenceObject(process);
    if (NT_SUCCESS(status))
        *information = (ULONG)request.command.size;
    return status;
}

// ---------------------------------------------------------------- dispatch

static NTSTATUS AbiCompleteIrp(PIRP irp, NTSTATUS status, ULONG_PTR information) {
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS AbiCreateClose(PDEVICE_OBJECT deviceObject, PIRP irp) {
    UNREFERENCED_PARAMETER(deviceObject);
    BOOLEAN acquired = ExAcquireRundownProtection(&g_RequestRundown);
    NTSTATUS status = STATUS_SUCCESS;
    if (IoGetCurrentIrpStackLocation(irp)->MajorFunction == IRP_MJ_CREATE &&
        (!acquired || !InterlockedCompareExchange(&g_DeviceCreated, 0, 0)))
        status = STATUS_DEVICE_NOT_READY;
    AbiCompleteIrp(irp, status, 0);
    if (acquired)
        ExReleaseRundownProtection(&g_RequestRundown);
    return status;
}

static VOID AbiDeleteLink(VOID) {
    if (g_LinkCreated) {
        UNICODE_STRING dosName;
        RtlInitUnicodeString(&dosName, ABI_DOS_NAME);
        IoDeleteSymbolicLink(&dosName);
        g_LinkCreated = FALSE;
    }
}

static NTSTATUS AbiDisableDevice(VOID) {
    if (!InterlockedExchange(&g_DeviceCreated, FALSE))
        return STATUS_DEVICE_NOT_READY;
    AbiDeleteLink();
    return STATUS_SUCCESS;
}

static NTSTATUS AbiDeviceControl(PDEVICE_OBJECT deviceObject, PIRP irp) {
    UNREFERENCED_PARAMETER(deviceObject);
    if (!ExAcquireRundownProtection(&g_RequestRundown))
        return AbiCompleteIrp(irp, STATUS_DELETE_PENDING, 0);

    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    ULONG controlCode = stack->Parameters.DeviceIoControl.IoControlCode;
    PVOID systemBuffer = irp->AssociatedIrp.SystemBuffer;
    ULONG inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    ULONG information = 0;

    __try {
        if (!InterlockedCompareExchange(&g_DeviceCreated, 0, 0)) {
            status = STATUS_DEVICE_NOT_READY;
        } else if (inputLength >= sizeof(ABI_COMMAND) && systemBuffer) {
            // Snapshot BEFORE any transfer: under METHOD_BUFFERED the
            // SystemBuffer IS the command storage — READ/WRITE overwrite it.
            ABI_COMMAND command = *(PABI_COMMAND)systemBuffer;

            if (command.magic_key != g_SessionKey) {
                status = STATUS_ACCESS_DENIED;
            } else if (controlCode == ABI_IOCTL_QUERY_STATUS) {
                if (outputLength < sizeof(ABI_STATUS)) {
                    status = STATUS_BUFFER_TOO_SMALL;
                } else {
                    ABI_STATUS result = { ABI_PROTOCOL_VERSION, TRUE, MAX_TRANSFER,
                        ABI_MAX_BATCH_COUNT, ABI_CAP_QUERY_STATUS |
                        ABI_CAP_PROCESS_INFO | ABI_CAP_READ_BATCH, 0 };
                    RtlCopyMemory(systemBuffer, &result, sizeof(result));
                    information = sizeof(result);
                    status = STATUS_SUCCESS;
                }
            } else if (controlCode == ABI_IOCTL_GET_PROCESS_INFO) {
                if (outputLength < sizeof(ABI_PROCESS_INFO)) {
                    status = STATUS_BUFFER_TOO_SMALL;
                } else {
                    ABI_PROCESS_INFO result;
                    status = GetTargetProcessInfo(command.process_id, &result);
                    if (NT_SUCCESS(status)) {
                        RtlCopyMemory(systemBuffer, &result, sizeof(result));
                        information = sizeof(result);
                    }
                }
            } else if (controlCode == ABI_IOCTL_READ_BATCH) {
                status = ReadTargetMemoryBatch(systemBuffer, inputLength,
                    outputLength, &information);
            } else if (controlCode == ABI_IOCTL_READ_MEMORY) {
                if (command.size == 0 || command.size > MAX_TRANSFER ||
                    command.size > outputLength) {
                    status = STATUS_INVALID_PARAMETER;
                } else {
                    status = ReadTargetMemory(command.process_id,
                        command.address, systemBuffer, (SIZE_T)command.size);
                    if (NT_SUCCESS(status))
                        information = command.size;
                }
            } else if (controlCode == ABI_IOCTL_WRITE_MEMORY) {
                if (command.size == 0 || command.size > MAX_TRANSFER ||
                    inputLength < sizeof(ABI_COMMAND) + command.size) {
                    status = STATUS_INVALID_PARAMETER;
                } else {
                    status = WriteTargetMemory(command.process_id,
                        command.address,
                        (PUCHAR)systemBuffer + sizeof(ABI_COMMAND),
                        (SIZE_T)command.size);
                }
            } else if (controlCode == ABI_IOCTL_GET_BASE) {
                ULONG_PTR base = 0;
                status = GetTargetImageBase(command.process_id, &base);
                if (NT_SUCCESS(status) && outputLength >= sizeof(ULONG_PTR)) {
                    *(ULONG_PTR*)systemBuffer = base;
                    information = sizeof(ULONG_PTR);
                }
            } else if (controlCode == ABI_IOCTL_UNLOAD) {
                // Stop accepting work. SCM stop unloads after clients close handles.
                status = AbiDisableDevice();
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        information = 0;
    }

    AbiCompleteIrp(irp, status, information);
    ExReleaseRundownProtection(&g_RequestRundown);
    return status;
}

// ---------------------------------------------------------------- load/unload

static DRIVER_UNLOAD AbiDriverUnload;

_Use_decl_annotations_
static VOID AbiDriverUnload(PDRIVER_OBJECT driverObject) {
    InterlockedExchange(&g_DeviceCreated, FALSE);
    ExWaitForRundownProtectionRelease(&g_RequestRundown);
    AbiDeleteLink();
    AbiDeleteSessionKey();
    g_SessionKey = 0;
    IoDeleteDevice(driverObject->DeviceObject);
}

DRIVER_INITIALIZE DriverEntry;

_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    UNREFERENCED_PARAMETER(registryPath);
    ExInitializeRundownProtection(&g_RequestRundown);
    g_SessionKey = ComputeSessionKey();

    driverObject->MajorFunction[IRP_MJ_CREATE] = AbiCreateClose;
    driverObject->MajorFunction[IRP_MJ_CLOSE] = AbiCreateClose;
    driverObject->MajorFunction[IRP_MJ_CLEANUP] = AbiCreateClose;
    driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = AbiDeviceControl;
    driverObject->DriverUnload = AbiDriverUnload;

    UNICODE_STRING deviceName, dosName, sddl;
    RtlInitUnicodeString(&deviceName, ABI_DEVICE_NAME);
    RtlInitUnicodeString(&dosName, ABI_DOS_NAME);
    RtlInitUnicodeString(&sddl, ABI_SDDL_DEVOBJ_SYS_ALL_AIX_RWX_WORLD_RX_RES);
    PDEVICE_OBJECT device = NULL;
    NTSTATUS status = IoCreateDeviceSecure(driverObject, 0, &deviceName,
        FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE,
        &sddl, &g_DeviceClass, &device);
    if (NT_SUCCESS(status)) {
        device->Flags |= DO_BUFFERED_IO;
        status = IoCreateSymbolicLink(&dosName, &deviceName);
        if (NT_SUCCESS(status)) {
            g_LinkCreated = TRUE;
            status = AbiPublishSessionKey();
        }
    }
    if (!NT_SUCCESS(status)) {
        AbiDeleteLink();
        AbiDeleteSessionKey();
        g_SessionKey = 0;
        if (device)
            IoDeleteDevice(device);
        return status;
    }

    InterlockedExchange(&g_DeviceCreated, TRUE);
    device->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
