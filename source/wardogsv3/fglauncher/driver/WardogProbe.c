                                                          
  
                                                                                 
                                                                         
                                                                               
                                                                              
  
                                                                               
                                                                           

typedef unsigned char       UCHAR;
typedef unsigned short      USHORT;
typedef unsigned long       ULONG;
typedef long                NTSTATUS;
typedef unsigned long long  ULONG_PTR;
typedef void*               PVOID;
typedef unsigned short      WCHAR;
typedef WCHAR*              PWSTR;

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

                                                                                
                                                                   
typedef struct _DRIVER_OBJECT {
    short  Type;                         
    short  Size;                         
    PVOID  DeviceObject;                 
    ULONG  Flags;                        
    PVOID  DriverStart;                  
    ULONG  DriverSize;                   
    PVOID  DriverSection;                
    PVOID  DriverExtension;              
    UNICODE_STRING DriverName;           
    PVOID  HardwareDatabase;             
    PVOID  FastIoDispatch;               
    PVOID  DriverInit;                   
    PVOID  DriverStartIo;                
    void (*DriverUnload)(struct _DRIVER_OBJECT* DriverObject);         
    PVOID  MajorFunction[28];            
} DRIVER_OBJECT, *PDRIVER_OBJECT;

NTSTATUS __stdcall IoCreateDevice(PDRIVER_OBJECT DriverObject, ULONG DeviceExtensionSize,
                                  PUNICODE_STRING DeviceName, ULONG DeviceType,
                                  ULONG DeviceCharacteristics, UCHAR Exclusive,
                                  PVOID* DeviceObject);
NTSTATUS __stdcall IoCreateSymbolicLink(PUNICODE_STRING SymbolicLinkName, PUNICODE_STRING DeviceName);
NTSTATUS __stdcall IoDeleteSymbolicLink(PUNICODE_STRING SymbolicLinkName);
void     __stdcall IoDeleteDevice(PVOID DeviceObject);

static WCHAR gDeviceNameBuf[] = L"\\Device\\WardogProbe";
static WCHAR gSymLinkBuf[]    = L"\\DosDevices\\WardogProbe";

static void InitUnicodeString(UNICODE_STRING* s, PWSTR buffer, USHORT chars) {
    s->Length = (USHORT)(chars * sizeof(WCHAR));
    s->MaximumLength = (USHORT)((chars + 1) * sizeof(WCHAR));
    s->Buffer = buffer;
}

static void WardogUnload(PDRIVER_OBJECT DriverObject) {
    UNICODE_STRING link;
    InitUnicodeString(&link, gSymLinkBuf, (USHORT)(sizeof(gSymLinkBuf) / sizeof(WCHAR) - 1));
    IoDeleteSymbolicLink(&link);
    if (DriverObject->DeviceObject) {
        IoDeleteDevice(DriverObject->DeviceObject);
        DriverObject->DeviceObject = 0;
    }
}

NTSTATUS __stdcall DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNICODE_STRING deviceName, linkName;
    PVOID deviceObject = 0;
    NTSTATUS status;

    (void)RegistryPath;
    InitUnicodeString(&deviceName, gDeviceNameBuf, (USHORT)(sizeof(gDeviceNameBuf) / sizeof(WCHAR) - 1));
    InitUnicodeString(&linkName, gSymLinkBuf, (USHORT)(sizeof(gSymLinkBuf) / sizeof(WCHAR) - 1));

                                                               
    status = IoCreateDevice(DriverObject, 0, &deviceName, 0x22, 0, 0, &deviceObject);
    if (status < 0) {
        return status;
    }
    status = IoCreateSymbolicLink(&linkName, &deviceName);
    if (status < 0) {
        IoDeleteDevice(deviceObject);
        DriverObject->DeviceObject = 0;
        return status;
    }
    DriverObject->DriverUnload = WardogUnload;
    return 0;
}
