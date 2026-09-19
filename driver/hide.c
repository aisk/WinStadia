// UMDF2 upper filter for the physical Stadia controller's HID collection.
//
// Hides the controller from games so that they only see the virtual
// DualShock 4: every open is denied unless the device path carries the
// suffix the winstadia app appends. HIDCLASS itself ignores that suffix.

#include <windows.h>
#include <wdf.h>

static const WCHAR BridgeSuffix[] = L"\\winstadia";

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_DEVICE_FILE_CREATE EvtDeviceFileCreate;

static BOOLEAN
IsBridgeOpen(WDFFILEOBJECT FileObject)
{
    PUNICODE_STRING name;

    if (FileObject == NULL) {
        return FALSE;
    }
    name = WdfFileObjectGetFileName(FileObject);
    return name != NULL && name->Buffer != NULL &&
           name->Length == sizeof(BridgeSuffix) - sizeof(WCHAR) &&
           _wcsnicmp(name->Buffer, BridgeSuffix, name->Length / sizeof(WCHAR)) == 0;
}

VOID
EvtDeviceFileCreate(WDFDEVICE Device, WDFREQUEST Request, WDFFILEOBJECT FileObject)
{
    WDF_REQUEST_SEND_OPTIONS options;

    if (!IsBridgeOpen(FileObject)) {
        WdfRequestComplete(Request, STATUS_ACCESS_DENIED);
        return;
    }

    WdfRequestFormatRequestUsingCurrentType(Request);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
    if (!WdfRequestSend(Request, WdfDeviceGetIoTarget(Device), &options)) {
        WdfRequestComplete(Request, WdfRequestGetStatus(Request));
    }
}

NTSTATUS
EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDFDEVICE device;

    UNREFERENCED_PARAMETER(Driver);

    // All other requests pass through to HIDCLASS untouched.
    WdfFdoInitSetFilter(DeviceInit);

    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, EvtDeviceFileCreate, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, WDF_NO_OBJECT_ATTRIBUTES);

    return WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
}

NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
