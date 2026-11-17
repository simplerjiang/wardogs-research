// ABI kernel driver v3 — kdmapper-mapped, OWN DEVICE (no null.sys hook).
//
// v2 hooked null.sys IRP_MJ_DEVICE_CONTROL — PatchGuard eventually fires
// 0x109 CRITICAL_STRUCTURE_CORRUPTION (two confirmed BSODs). v3 creates a
// proper DRIVER_OBJECT via IoCreateDriver + own device \Device\AbiComm with
// symlink \??\AbiComm. Creating new objects is outside PatchGuard's scope
// (PG polices existing kernel code/tables, not the object namespace).
//
// kdmapper calls entry on a hijacked thread: entry ONLY spawns a system
// thread; IoCreateDriver + IoCreateDevice run there at PASSIVE with a clean
// context (file opens / object manager calls page-faulted from the hijacked
// thread — that was the 0x50 BSOD).
//
// Client contract (ABI/includes/driver/driver.hpp): open \\.\AbiComm,
// METHOD_BUFFERED IOCTLs, ABI_COMMAND {key, pid, addr, size}.
// Session key: PrefetchParameters\BootId ^ 0x8F2A9C4B, xorshift32.

#include <ntddk.h>

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

NTKERNELAPI
NTSTATUS
PsLookupProcessByProcessId(
    HANDLE ProcessId,
    PEPROCESS* Process
);

// wdmsec.lib exports available in this WDK (no IoCreateDeviceSecure there).
// IoCreateUnprotectedSymbolicLink creates a DOS link without ACL checks —
// exactly what a mapped driver needs for a world-open device.
NTSTATUS
IoCreateUnprotectedSymbolicLink(
    PUNICODE_STRING SymbolicLinkName,
    PUNICODE_STRING DeviceName
);

NTKERNELAPI
NTSTATUS
IoCreateDriver(
    PUNICODE_STRING DriverName,
    PDRIVER_INITIALIZE InitializationFunction
);

#define KEY_SALT 0x8F2A9C4BUL

// Must match CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80x, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ABI_IOCTL_READ_MEMORY   0x222004
#define ABI_IOCTL_GET_BASE      0x222008
#define ABI_IOCTL_UNLOAD        0x22200C
#define ABI_IOCTL_WRITE_MEMORY  0x222010

#define MAX_TRANSFER 0x10000

// SDDL for IoCreateDeviceSecure: system full access, world read/execute.
// (wdmsec.h isn't in our include set; the canonical string is stable.)
#define ABI_SDDL_DEVOBJ_SYS_ALL_AIX_RWX_WORLD_RX_RES \
    L"D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)(A;;GRGWGX;;;WD)(A;;GRGWGX;;;RC)"

#pragma pack(push, 1)
typedef struct _ABI_COMMAND {
    ULONG     magic_key;
    ULONG     process_id;
    ULONG_PTR address;
    SIZE_T    size;
} ABI_COMMAND, *PABI_COMMAND;
#pragma pack(pop)

static ULONG  g_SessionKey = 0;
static BOOLEAN g_DeviceCreated = FALSE;
static PDEVICE_OBJECT g_DeviceObject = NULL;

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

// ---------------------------------------------------------------- memory ops

static NTSTATUS CopyCrossProcess(PEPROCESS sourceProcess, ULONG_PTR sourceAddress,
    PEPROCESS targetProcess, ULONG_PTR targetAddress, SIZE_T size, PSIZE_T transferred) {
    return MmCopyVirtualMemory(sourceProcess, (PVOID)sourceAddress,
        targetProcess, (PVOID)targetAddress, size, KernelMode, transferred);
}

static NTSTATUS ReadTargetMemory(ULONG processId, ULONG_PTR address,
    PVOID kernelBuffer, SIZE_T size) {
    PEPROCESS process = NULL;
    NTSTATUS status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)processId, &process);
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
    PEPROCESS process = NULL;
    NTSTATUS status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)processId, &process);
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

static NTSTATUS GetTargetImageBase(ULONG processId, PULONG_PTR imageBase) {
    *imageBase = 0;

    PEPROCESS process = NULL;
    NTSTATUS status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)processId, &process);
    if (!NT_SUCCESS(status))
        return status;

    PPEB peb = PsGetProcessPeb(process);
    if (!peb) {
        ObDereferenceObject(process);
        return STATUS_UNSUCCESSFUL;
    }

    SIZE_T transferred = 0;
    status = CopyCrossProcess(process, (ULONG_PTR)peb + 0x10,
        PsGetCurrentProcess(), (ULONG_PTR)imageBase, sizeof(ULONG_PTR), &transferred);
    ObDereferenceObject(process);

    if (NT_SUCCESS(status) && transferred != sizeof(ULONG_PTR))
        return STATUS_PARTIAL_COPY;
    return status;
}

// ---------------------------------------------------------------- dispatch

static NTSTATUS AbiCreateClose(PDEVICE_OBJECT deviceObject, PIRP irp) {
    UNREFERENCED_PARAMETER(deviceObject);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

// Deferred symlink keeper: kdmapper's trace cleanup runs AFTER our init
// thread and can wipe freshly created \?? objects (symptom: CreateFile 433
// with InitStatus=0). A system thread sleeps 15s (mapper long gone), then
// (re)creates the DOS symlink every 30s in case anything else removes it.
// ObQueryNameString: no prototype reaches us from the WDK headers here, so
// MSVC was inferring "int ObQueryNameString(...)" and marshalling the
// arguments incorrectly. That mismatch faults inside the keeper thread and
// surfaces as 0x7E SYSTEM_THREAD_EXCEPTION_NOT_HANDLED. Declare it properly.
NTKERNELAPI
NTSTATUS
ObQueryNameString(
    _In_ PVOID Object,
    _Out_writes_bytes_opt_(Length) POBJECT_NAME_INFORMATION ObjectNameInfo,
    _In_ ULONG Length,
    _Out_ PULONG ReturnLength
    );

static VOID AbiSymlinkThread(PVOID context) {
    PDEVICE_OBJECT device = (PDEVICE_OBJECT)context;

    LARGE_INTEGER interval;
    interval.QuadPart = -15000 * 10000LL; // 15s relative (100ns units, negative)
    KeDelayExecutionThread(KernelMode, FALSE, &interval);

    for (;;) {
        /* ObQueryNameString must run at IRQL <= APC_LEVEL and can raise. A
           system thread has no frame above us, so an unguarded exception here
           becomes 0x7E SYSTEM_THREAD_EXCEPTION_NOT_HANDLED and takes the whole
           box down. Guard it and skip the pass entirely if IRQL is too high. */
        if (KeGetCurrentIrql() <= APC_LEVEL) {
            __try {
                UNICODE_STRING dosName;
                RtlInitUnicodeString(&dosName, L"\\??\\AbiComm");

                // Verify-first: only touch the namespace when the link is
                // really gone. Delete+recreate every 30s is a churn signature
                // object-manager scans pick up (open handles by symlink
                // target, reference counts flapping in the directory).
                PWCHAR buf = ExAllocatePoolWithTag(NonPagedPool, 0x100, 'ibaD');
                if (buf) {
                    POBJECT_NAME_INFORMATION nameInfo = (POBJECT_NAME_INFORMATION)buf;
                    ULONG retLen = 0;
                    NTSTATUS qstatus = ObQueryNameString(device, nameInfo, 0x100, &retLen);
                    BOOLEAN linkOk = FALSE;
                    if (NT_SUCCESS(qstatus) && nameInfo->Name.Buffer && nameInfo->Name.Length > 0) {
                        // The link exists if a query of "\??\AbiComm" resolves
                        // back to our device name. Opening the symlink read-only
                        // is the cheapest authoritative check.
                        OBJECT_ATTRIBUTES oa;
                        HANDLE lnk = NULL;
                        InitializeObjectAttributes(&oa, &dosName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
                        if (NT_SUCCESS(ZwOpenSymbolicLinkObject(&lnk, GENERIC_READ, &oa))) {
                            WCHAR target[128];
                            ULONG tl = 0;
                            UNICODE_STRING tus;
                            tus.Buffer = target; tus.Length = 0; tus.MaximumLength = sizeof(target);
                            if (NT_SUCCESS(ZwQuerySymbolicLinkObject(lnk, &tus, &tl))) {
                                // match on the device suffix; name = \Device\AbiCommN
                                linkOk = (tus.Length >= nameInfo->Name.Length) &&
                                         (wcsncmp(tus.Buffer, nameInfo->Name.Buffer,
                                                  nameInfo->Name.Length / sizeof(WCHAR)) == 0 ||
                                          wcsstr(target, L"AbiComm") != NULL);
                            }
                            ZwClose(lnk);
                        }
                    }
                    if (!linkOk && NT_SUCCESS(qstatus) && nameInfo->Name.Buffer && nameInfo->Name.Length > 0) {
                        NTSTATUS status = IoCreateUnprotectedSymbolicLink(&dosName, &nameInfo->Name);
                        (VOID)status;
                    }
                    ExFreePoolWithTag(buf, 'ibaD');
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        interval.QuadPart = -30000 * 10000LL; // 30s
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }
    // never exits: mapped-image driver has no unload path; thread dies with
    // the system session (acceptable: it only sleeps + touches one symlink)
}

static NTSTATUS AbiDeviceControl(PDEVICE_OBJECT deviceObject, PIRP irp) {
    UNREFERENCED_PARAMETER(deviceObject);

    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    ULONG controlCode = stack->Parameters.DeviceIoControl.IoControlCode;
    PVOID systemBuffer = irp->AssociatedIrp.SystemBuffer;
    ULONG inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    ULONG information = 0;

    __try {
        if (inputLength >= sizeof(ABI_COMMAND) && systemBuffer) {
            // Snapshot BEFORE any transfer: under METHOD_BUFFERED the
            // SystemBuffer IS the command storage — READ/WRITE overwrite it.
            ABI_COMMAND command = *(PABI_COMMAND)systemBuffer;

            if (command.magic_key != g_SessionKey) {
                status = STATUS_ACCESS_DENIED;
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
                // Tear down symlink; device stays (mapped image has no
                // unload) but becomes unreachable for new opens.
                UNICODE_STRING dosName;
                RtlInitUnicodeString(&dosName, L"\\??\\AbiComm");
                IoDeleteSymbolicLink(&dosName);
                g_DeviceCreated = FALSE;
                status = STATUS_SUCCESS;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        information = 0;
    }

    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

// ---------------------------------------------------------------- driver init

static NTSTATUS AbiDriverInit(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    UNREFERENCED_PARAMETER(registryPath);

    UNICODE_STRING deviceName;
    PDEVICE_OBJECT device = NULL;
    NTSTATUS status;

    // Device name collision (previous mapping this boot): probe suffixes.
    status = STATUS_OBJECT_NAME_COLLISION;
    for (ULONG attempt = 0; attempt < 64 && status == STATUS_OBJECT_NAME_COLLISION; attempt++) {
        WCHAR nameBuf[48] = { 0 };
        const WCHAR base[] = L"\\Device\\AbiComm";
        USHORT len = (USHORT)(sizeof(base) / sizeof(WCHAR) - 1);
        USHORT pos = len;
        ULONG val = attempt;
        WCHAR digits[12];
        int dn = 0;
        do { digits[dn++] = (WCHAR)(L'0' + (val % 10)); val /= 10; } while (val);
        while (dn > 0 && pos + 1 < 48) nameBuf[pos++] = digits[--dn];
        nameBuf[pos] = L'\0';
        for (USHORT i = 0; i < len; i++) nameBuf[i] = base[i];
        RtlInitUnicodeString(&deviceName, nameBuf);

        status = IoCreateDevice(driverObject, 0, &deviceName, FILE_DEVICE_UNKNOWN,
            FILE_DEVICE_SECURE_OPEN, FALSE, &device);
    }
    if (!NT_SUCCESS(status)) {

        return status;
    }

    device->Flags |= DO_BUFFERED_IO;
    device->Flags &= ~DO_DEVICE_INITIALIZING;

    // One global DOS symlink — handled by AbiSymlinkThread (deferred; the
    // mapper's trace cleanup wipes \?? objects created before it exits).

    driverObject->MajorFunction[IRP_MJ_CREATE]         = AbiCreateClose;
    driverObject->MajorFunction[IRP_MJ_CLOSE]          = AbiCreateClose;
    driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = AbiDeviceControl;

    g_DeviceObject = device;
    g_DeviceCreated = TRUE;

    // Keeper thread: (re)creates the symlink every 30s, starting at +15s.
    {
        HANDLE keeper = NULL;
        if (NT_SUCCESS(PsCreateSystemThread(&keeper, THREAD_ALL_ACCESS,
                NULL, NULL, NULL, AbiSymlinkThread, device))) {
            ZwClose(keeper);
        }
    }

    return STATUS_SUCCESS;
}


// ---------------------------------------------------------------- stealth
// "Kernel Image Integrity" scans walk executable anonymous regions looking for
// PE headers ("MZ" + e_lfa). A mapped image keeps its DOS/NT headers in place;
// zeroing them makes the region look like JIT scratch memory. Safe: nothing
// references our own headers at runtime (no imports left, no TLS, no exceptions
// unwinding into .rdata once init is done).
typedef struct _ABI_IMAGE_DOS_HEADER {
    USHORT e_magic; USHORT e_cblp; USHORT e_cp; USHORT e_crlc; USHORT e_cparhdr;
    USHORT e_minalloc; USHORT e_maxalloc; USHORT e_ss; USHORT e_sp; USHORT e_csum;
    USHORT e_ip; USHORT e_cs; USHORT e_lfarlc; USHORT e_ovno; USHORT e_res[4];
    USHORT e_oemid; USHORT e_oeminfo; USHORT e_res2[10]; LONG e_lfanew;
} ABI_IMAGE_DOS_HEADER;

static VOID WipeImageHeader(VOID) {
    // Locate the mapped image base by walking back from this function's code.
    // Mapped images are 0x1000-aligned, so round the instruction pointer down.
    ULONG_PTR ip = (ULONG_PTR)(PVOID)&WipeImageHeader;
    ULONG_PTR base = ip & ~0xFFFULL;
    for (int i = 0; i < 64; i++) {
        if (base < 0x10000) return;
        __try {
            ABI_IMAGE_DOS_HEADER dos;
            RtlCopyMemory(&dos, (PVOID)base, sizeof(dos));
            if (dos.e_magic == 0x5A4D && dos.e_lfanew > 0 && dos.e_lfanew < 0x400) {
                // Validate the NT signature ('PE\0\0') before wiping.
                ULONG ntSig = 0;
                RtlCopyMemory(&ntSig, (PVOID)(base + dos.e_lfanew), sizeof(ntSig));
                if (ntSig == 0x00004550) {
                    ULONG wipeLen = (ULONG)dos.e_lfanew + 0x400;
                    if (wipeLen > 0x1000) wipeLen = 0x1000;
                    RtlZeroMemory((PVOID)base, wipeLen);
                    return;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        base -= 0x1000;
    }
}

// ---------------------------------------------------------------- init thread

static VOID AbiDiag(ULONG step, NTSTATUS st) {
    UNICODE_STRING keyPath;
    OBJECT_ATTRIBUTES attributes;
    HANDLE keyHandle = NULL;
    RtlInitUnicodeString(&keyPath, L"\Registry\Machine\SYSTEM\AbiComm");
    InitializeObjectAttributes(&attributes, &keyPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateKey(&keyHandle, KEY_SET_VALUE, &attributes, 0,
            NULL, REG_OPTION_VOLATILE, NULL))) {
        char vn[32];
        UNICODE_STRING vns;
        // "S%u" имя по шагу
        int n = 0;
        vn[n++] = 'S'; vn[n++] = '0' + (char)(step % 10);
        vn[n] = 0;
        RtlInitUnicodeString(&vns, L"Step");
        ZwSetValueKey(keyHandle, &vns, 0, REG_DWORD, &st, sizeof(st));
        ZwClose(keyHandle);
    }
}

static VOID AbiInitThread(PVOID context) {
    UNREFERENCED_PARAMETER(context);

    AbiDiag(1, 0); // поток жив

    // Clean system-thread context: registry + object namespace ops are legal.
    g_SessionKey = ComputeSessionKey();
    AbiDiag(2, 0); // ключ посчитан

    // IoCreateDriver fails with STATUS_OBJECT_NAME_COLLISION if the name
    // already exists (previous mapping this boot, no unload path for it).
    // Probe sequential suffixes until a free name is found.
    NTSTATUS status = STATUS_OBJECT_NAME_COLLISION;
    for (ULONG attempt = 0; attempt < 64 && status == STATUS_OBJECT_NAME_COLLISION; attempt++) {
        // manual: "\Driver\AbiComm" + decimal suffix (no swprintf in kernel)
        WCHAR nameBuf[48] = { 0 };
        UNICODE_STRING driverName;
        const WCHAR base[] = L"\\Driver\\AbiComm";
        USHORT len = (USHORT)(sizeof(base) / sizeof(WCHAR) - 1);
        USHORT pos = len;
        ULONG val = attempt;
        WCHAR digits[12];
        int dn = 0;
        do { digits[dn++] = (WCHAR)(L'0' + (val % 10)); val /= 10; } while (val);
        while (dn > 0 && pos + 1 < 48) nameBuf[pos++] = digits[--dn];
        nameBuf[pos] = L'\0';
        for (USHORT i = 0; i < len; i++) nameBuf[i] = base[i];
        RtlInitUnicodeString(&driverName, nameBuf);

        status = IoCreateDriver(&driverName, AbiDriverInit);
        AbiDiag(3, status); // после IoCreateDriver
    }

    // Success: scrub our own PE header so image-integrity scans of anonymous
    // kernel regions find no PE signature. After this point the mapped region
    // is indistinguishable from pooled executable memory.
    if (NT_SUCCESS(status)) {
        WipeImageHeader();
    }

    // Persist init status for usermode diagnostics (no debugger attached).
    {
        UNICODE_STRING keyPath, valName;
        OBJECT_ATTRIBUTES attributes;
        HANDLE keyHandle = NULL;
        RtlInitUnicodeString(&keyPath, L"\\Registry\\Machine\\SYSTEM\\AbiComm");
        InitializeObjectAttributes(&attributes, &keyPath,
            OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(ZwCreateKey(&keyHandle, KEY_SET_VALUE, &attributes, 0,
                NULL, REG_OPTION_VOLATILE, NULL))) {
            RtlInitUnicodeString(&valName, L"InitStatus");
            ULONG st = (ULONG)status;
            ZwSetValueKey(keyHandle, &valName, 0, REG_DWORD, &st, sizeof(st));
            RtlInitUnicodeString(&valName, L"SessionKey");
            ULONG sk = g_SessionKey;
            ZwSetValueKey(keyHandle, &valName, 0, REG_DWORD, &sk, sizeof(sk));
            ZwClose(keyHandle);
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

// ---------------------------------------------------------------- entry

// kdmapper hijacked-thread context: NOTHING beyond thread creation is safe.
NTSTATUS DriverEntry(PVOID parameterOne, PVOID parameterTwo) {
    UNREFERENCED_PARAMETER(parameterOne);
    UNREFERENCED_PARAMETER(parameterTwo);

    HANDLE thread = NULL;
    NTSTATUS status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS,
        NULL, NULL, NULL, AbiInitThread, NULL);
    if (!NT_SUCCESS(status))
        return status;

    ZwClose(thread);
    return STATUS_SUCCESS;
}
