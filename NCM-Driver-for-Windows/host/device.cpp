// Copyright (C) Microsoft Corporation. All rights reserved.

#include "driver.h"
#include "device.tmh"

#define MAX_HOST_NTB_SIZE               (0x10000)
#define MAX_HOST_MTU_SIZE               (9014)
#define MAX_HOST_TX_NTB_DATAGRAM_COUNT  (UINT16) (16)
#define PENDING_BULK_IN_READS           (8)

const USBNCM_DEVICE_EVENT_CALLBACKS UsbNcmHostDevice::s_NcmDeviceCallbacks =
{
    sizeof(USBNCM_DEVICE_EVENT_CALLBACKS),
    UsbNcmHostDevice::StartReceive,
    UsbNcmHostDevice::StopReceive,
    UsbNcmHostDevice::StartTransmit,
    UsbNcmHostDevice::StopTransmit,
    UsbNcmHostDevice::TransmitFrames
};

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
StartPipe(
    _In_ WDFUSBPIPE pipe
)
{
    // Guard against NULL pipe
    if (pipe == nullptr)
    {
        return STATUS_SUCCESS;
    }
    
    WDFIOTARGET wdfIotarget;

    wdfIotarget = WdfUsbTargetPipeGetIoTarget(pipe);
    return WdfIoTargetStart(wdfIotarget);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
StopPipe(
    _In_ WDFUSBPIPE pipe
)
{
    // Guard against NULL pipe
    if (pipe == nullptr)
    {
        return;
    }
    
    WDFIOTARGET wdfIotarget;

    wdfIotarget = WdfUsbTargetPipeGetIoTarget(pipe);
    WdfIoTargetStop(wdfIotarget, WdfIoTargetCancelSentIo);
}

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::SetDeviceFriendlyName(
    void
)
{
    //  Update the device name with the model from the USB descriptor
    USB_DEVICE_DESCRIPTOR deviceDescriptor;
    PWSTR friendlyName = nullptr;
    WDFMEMORY friendlyNameMemory;
    WDF_OBJECT_ATTRIBUTES objectAttribs;

    WdfUsbTargetDeviceGetDeviceDescriptor(m_WdfUsbTargetDevice, &deviceDescriptor);

    USHORT manufacturerStringLength = 0;
    USHORT productStringLength = 0;

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfUsbTargetDeviceQueryString(
            m_WdfUsbTargetDevice,
            nullptr,
            nullptr,
            nullptr,
            &manufacturerStringLength,
            deviceDescriptor.iManufacturer,
            0),
        "WdfUsbTargetDeviceQueryString failed");

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfUsbTargetDeviceQueryString(
            m_WdfUsbTargetDevice,
            nullptr,
            nullptr,
            nullptr,
            &productStringLength,
            deviceDescriptor.iProduct,
            0),
        "WdfUsbTargetDeviceQueryString failed");

    ULONG friendlyNameByteCount = sizeof(WCHAR) * 
        (manufacturerStringLength + 1 +  // 1 white space
         productStringLength + 1);       // allocate 1 more char to make sure string would be null-terminated
   
    WDF_OBJECT_ATTRIBUTES_INIT(&objectAttribs);
    objectAttribs.ParentObject = m_WdfDevice;

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfMemoryCreate(
            &objectAttribs,
            PagedPool,
            0,
            friendlyNameByteCount,
            &friendlyNameMemory,
            (PVOID *)&friendlyName),
        "WdfMemoryCreate failed");

    RtlZeroMemory(friendlyName, friendlyNameByteCount);

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfUsbTargetDeviceQueryString(
            m_WdfUsbTargetDevice,
            nullptr,
            nullptr,
            friendlyName,
            &manufacturerStringLength,
            deviceDescriptor.iManufacturer,
            0),
        "WdfUsbTargetDeviceQueryString failed");

    friendlyName[manufacturerStringLength] = L' ';
    
    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfUsbTargetDeviceQueryString(
            m_WdfUsbTargetDevice,
            nullptr,
            nullptr,
            &friendlyName[manufacturerStringLength + 1],
            &productStringLength,
            deviceDescriptor.iProduct,
            0),
        "WdfUsbTargetDeviceQueryString failed");
 
    WDF_DEVICE_PROPERTY_DATA propertyData;
    WDF_DEVICE_PROPERTY_DATA_INIT(&propertyData, &DEVPKEY_Device_FriendlyName);
    propertyData.Flags = PLUGPLAY_PROPERTY_PERSISTENT;

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfDeviceAssignProperty(
            m_WdfDevice,
            &propertyData,
            DEVPROP_TYPE_STRING,
            friendlyNameByteCount,
            friendlyName),
        "WdfDeviceAssignProperty failed");

    return STATUS_SUCCESS;
}

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::InitializeDevice(
    void
)
{
    WDF_USB_DEVICE_INFORMATION deviceInfo;
    WDF_USB_DEVICE_CREATE_CONFIG createParams;
    NTSTATUS status;

    PAGED_CODE();

    WDF_USB_DEVICE_CREATE_CONFIG_INIT(
        &createParams,
        USBD_CLIENT_CONTRACT_VERSION_602);

    status = WdfUsbTargetDeviceCreateWithParameters(
        m_WdfDevice,
        &createParams,
        WDF_NO_OBJECT_ATTRIBUTES,
        &m_WdfUsbTargetDevice);
    if (!NT_SUCCESS(status))
    {
        DbgPrint("USBNCM: WdfUsbTargetDeviceCreateWithParameters FAILED 0x%08X\n", status);
        return status;
    }

    // Detect Apple Mac NCM device (VID 0x05AC, PID 0x1905)
    USB_DEVICE_DESCRIPTOR deviceDescriptor;
    WdfUsbTargetDeviceGetDeviceDescriptor(m_WdfUsbTargetDevice, &deviceDescriptor);
    
    if (deviceDescriptor.idVendor == 0x05AC && deviceDescriptor.idProduct == 0x1905)
    {
        m_IsAppleDevice = TRUE;
        DbgPrint("USBNCM: Apple Mac USB NCM device detected (VID=%04X PID=%04X)\n",
            deviceDescriptor.idVendor, deviceDescriptor.idProduct);
    }

    // Check what interfaces Windows gave us
    BYTE numInterfaces = WdfUsbTargetDeviceGetNumInterfaces(m_WdfUsbTargetDevice);

    BOOLEAN hasControlInterface = FALSE;
    BOOLEAN hasDataInterface = FALSE;
    
    // For Apple devices with composite parent match, we should get ALL interfaces
    // Look for the FIRST NCM pair: Interface 0 (control) + Interface 1 (data)
    for (BYTE i = 0; i < numInterfaces; i++)
    {
        WDFUSBINTERFACE usbInterface = WdfUsbTargetDeviceGetInterface(m_WdfUsbTargetDevice, i);
        USB_INTERFACE_DESCRIPTOR ifDesc;
        WdfUsbInterfaceGetDescriptor(usbInterface, 0, &ifDesc);
        
        BYTE ifNum = WdfUsbInterfaceGetInterfaceNumber(usbInterface);
        // For Apple composite device: Use interface 0 (control) and interface 1 (data)
        // (The device also has interface 2+3 as a second NCM pair, but we only use the first)
        if (m_IsAppleDevice)
        {
            if (ifNum == 0 && ifDesc.bInterfaceClass == 0x02 && ifDesc.bInterfaceSubClass == 0x0D)
            {
                hasControlInterface = TRUE;
                m_ControlInterface = usbInterface;
            }
            else if (ifNum == 1 && ifDesc.bInterfaceClass == 0x0A)
            {
                hasDataInterface = TRUE;
                m_DataInterface = usbInterface;
            }
        }
        else
        {
            // Standard NCM device - just take first control and first data
            if (ifDesc.bInterfaceClass == 0x02 && ifDesc.bInterfaceSubClass == 0x0D && !hasControlInterface)
            {
                hasControlInterface = TRUE;
                m_ControlInterface = usbInterface;
            }
            else if (ifDesc.bInterfaceClass == 0x0A && !hasDataInterface)
            {
                hasDataInterface = TRUE;
                m_DataInterface = usbInterface;
            }
        }
    }

    // For Apple devices: Check if Windows split the interfaces
    if (m_IsAppleDevice)
    {
        if (numInterfaces >= 4)
        {
            // Got the composite device - all interfaces available!
            DbgPrint("USBNCM: Apple composite device with %d interfaces\n", numInterfaces);
        }
        else if (hasControlInterface && !hasDataInterface)
        {
            // Windows gave us ONLY the control interface - this is the split problem
            DbgPrint("USBNCM: ERROR - Windows split interfaces, need composite parent match\n");
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        
        if (!hasControlInterface && hasDataInterface)
        {
            // This is a data interface - just be a placeholder
            m_IsDataInterfaceOnly = TRUE;
            return STATUS_SUCCESS;
        }
    }

    // Standard NCM path - we have both interfaces
    
    // Ignore any error if we failed to set PnP FriendlyName
    (void) SetDeviceFriendlyName();

    WDF_USB_DEVICE_INFORMATION_INIT(&deviceInfo);

    status = WdfUsbTargetDeviceRetrieveInformation(
        m_WdfUsbTargetDevice,
        &deviceInfo);
    if (!NT_SUCCESS(status))
    {
        DbgPrint("USBNCM: WdfUsbTargetDeviceRetrieveInformation FAILED 0x%08X\n", status);
        return status;
    }

    status = SelectConfiguration();
    if (!NT_SUCCESS(status))
    {
        DbgPrint("USBNCM: SelectConfiguration FAILED 0x%08X\n", status);
        return status;
    }

    status = SelectSetting();
    if (!NT_SUCCESS(status))
    {
        DbgPrint("USBNCM: SelectSetting FAILED 0x%08X\n", status);
        return status;
    }

    status = RetrieveInterruptPipe();
    if (!NT_SUCCESS(status))
    {
        DbgPrint("USBNCM: RetrieveInterruptPipe FAILED 0x%08X\n", status);
        return status;
    }

    status = RetrieveDataBulkPipes();
    if (!NT_SUCCESS(status))
    {
        DbgPrint("USBNCM: RetrieveDataBulkPipes FAILED 0x%08X\n", status);
        return status;
    }

    // Final validation
    if (!m_DataBulkInPipe || !m_DataBulkOutPipe)
    {
        DbgPrint("USBNCM: Missing bulk pipes - cannot function\n");
        return STATUS_DEVICE_HARDWARE_ERROR;
    }
    
    if (!m_IsAppleDevice && !m_ControlInterruptPipe)
    {
        DbgPrint("USBNCM: Missing interrupt pipe for non-Apple device\n");
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    DbgPrint("USBNCM: InitializeDevice SUCCESS\n");
    return STATUS_SUCCESS;
}

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::CreateAdapter(
    void
)
{
    PAGED_CODE();

    USBNCM_ADAPTER_PARAMETERS parameters =
    {
        m_Use32BitNtb,
        m_MacAddress,
        m_MaxDatagramSize,
        m_NtbParamters.wNtbOutMaxDatagrams > 0
            ? m_NtbParamters.wNtbOutMaxDatagrams
            : MAX_HOST_TX_NTB_DATAGRAM_COUNT,
        m_NtbParamters.dwNtbOutMaxSize,
        m_NtbParamters.wNdpOutAlignment,
        m_NtbParamters.wNdpOutDivisor,
        m_NtbParamters.wNdpOutPayloadRemainder,
    };

    NCM_RETURN_IF_NOT_NT_SUCCESS(
        UsbNcmAdapterCreate(
            m_WdfDevice,
            &parameters,
            &UsbNcmHostDevice::s_NcmDeviceCallbacks,
            &m_NetAdapter,
            &m_NcmAdapterCallbacks));

    return STATUS_SUCCESS;
}

PAGEDX
_Use_decl_annotations_
void
UsbNcmHostDevice::DestroyAdapter(
    void
)
{
    PAGED_CODE();

    if (m_NetAdapter != nullptr)
    {
        UsbNcmAdapterDestory(m_NetAdapter);
    }
}

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::RequestClassSpecificControlTransfer(
    UINT8 request,
    WDF_USB_BMREQUEST_DIRECTION direction,
    WDF_USB_BMREQUEST_RECIPIENT recipient,
    UINT16 value,
    PWDF_MEMORY_DESCRIPTOR memoryDescriptor
)
{
    WDF_USB_CONTROL_SETUP_PACKET controlSetupPacket;
    WDF_REQUEST_SEND_OPTIONS sendOptions;

    PAGED_CODE();

    WDF_USB_CONTROL_SETUP_PACKET_INIT_CLASS(
        &controlSetupPacket,
        direction,
        recipient,
        request,
        value,
        WdfUsbInterfaceGetInterfaceNumber(m_ControlInterface));

    WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, 0);

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfUsbTargetDeviceSendControlTransferSynchronously(
            m_WdfUsbTargetDevice,
            WDF_NO_HANDLE,
            &sendOptions,
            &controlSetupPacket,
            memoryDescriptor,
            nullptr),
        "WdfUsbTargetDeviceSendControlTransferSynchronously failed");

    return STATUS_SUCCESS;
}

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::SelectConfiguration(
    void
)
{
    WDF_OBJECT_ATTRIBUTES objectAttribs;

    PAGED_CODE();

    NTSTATUS status = STATUS_SUCCESS;

    // Get configuration descriptor
    PUSB_CONFIGURATION_DESCRIPTOR pDescriptors = NULL;
    USHORT sizeDescriptors;
    WDFMEMORY descriptorMemory;

    status = WdfUsbTargetDeviceRetrieveConfigDescriptor(
        m_WdfUsbTargetDevice,
        NULL,
        &sizeDescriptors);

    NCM_RETURN_NT_STATUS_IF_FALSE_MSG(
        status == STATUS_BUFFER_TOO_SMALL,
        status,
        "WdfUsbTargetDeviceRetrieveConfigDescriptor failed");

    WDF_OBJECT_ATTRIBUTES_INIT(&objectAttribs);
    objectAttribs.ParentObject = m_WdfDevice;

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfMemoryCreate(
            &objectAttribs,
            NonPagedPoolNx,
            0,
            sizeDescriptors,
            &descriptorMemory,
            (PVOID*) &pDescriptors),
        "WdfMemoryCreate failed");

    RtlZeroMemory(pDescriptors, sizeDescriptors);

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfUsbTargetDeviceRetrieveConfigDescriptor(
            m_WdfUsbTargetDevice,
            pDescriptors,
            &sizeDescriptors),
        "WdfUsbTargetDeviceRetrieveConfigDescriptor failed");

    // Verify data interface has 2 alternate settings
    BYTE numSettings = WdfUsbInterfaceGetNumSettings(m_DataInterface);
    if (numSettings != 2)
    {
        DbgPrint("USBNCM: ERROR - Data interface needs 2 alt settings, has %d\n", numSettings);
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    // Select configuration - method depends on whether we have composite device
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS configParams;
    BYTE numInterfaces = WdfUsbTargetDeviceGetNumInterfaces(m_WdfUsbTargetDevice);
    
    if (m_IsAppleDevice && numInterfaces == 4)
    {
        // Apple composite device: we have all 4 interfaces, configure them all
        // Interface 0: NCM Control (setting 0)
        // Interface 1: NCM Data (setting 0, will switch to 1 later)
        // Interface 2: NCM Control (setting 0) - unused
        // Interface 3: NCM Data (setting 0) - unused
        WDFUSBINTERFACE if2 = WdfUsbTargetDeviceGetInterface(m_WdfUsbTargetDevice, 2);
        WDFUSBINTERFACE if3 = WdfUsbTargetDeviceGetInterface(m_WdfUsbTargetDevice, 3);
        
        WDF_USB_INTERFACE_SETTING_PAIR settingPairs[4] =
        {
            {m_ControlInterface, 0},  // IF#0 - control
            {m_DataInterface, 0},     // IF#1 - data (alt setting 0)
            {if2, 0},                 // IF#2 - control (unused)
            {if3, 0}                  // IF#3 - data (unused)
        };
        
        WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_MULTIPLE_INTERFACES(
            &configParams,
            4,
            settingPairs);
        
    }
    else
    {
        // Standard NCM device: just 2 interfaces
        WDF_USB_INTERFACE_SETTING_PAIR settingPair[2] =
        {
            {m_ControlInterface, 0},
            {m_DataInterface, 0}
        };

        WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_MULTIPLE_INTERFACES(
            &configParams,
            ARRAYSIZE(settingPair),
            settingPair);
    }

    status = WdfUsbTargetDeviceSelectConfig(
        m_WdfUsbTargetDevice,
        WDF_NO_OBJECT_ATTRIBUTES,
        &configParams);
    if (!NT_SUCCESS(status))
    {
        DbgPrint("USBNCM: SelectConfig FAILED 0x%08X\n", status);
        return status;
    }

    // Parse functional descriptors from OUR control interface
    PUSB_NCM_CS_FUNCTIONAL_DESCRIPTOR pNcmFunctionalDescr = nullptr;
    PUSB_ECM_CS_NET_FUNCTIONAL_DESCRIPTOR pEcmFunctionalDescr = nullptr;
    BYTE controlInterfaceNumber = WdfUsbInterfaceGetInterfaceNumber(m_ControlInterface);
    BYTE currentInterfaceNumber = 0xFF;

    size_t currDescrptorOffset = 0;
    PUSB_COMMON_DESCRIPTOR pCurrDescriptor = (PUSB_COMMON_DESCRIPTOR)pDescriptors;

    while (currDescrptorOffset < pDescriptors->wTotalLength)
    {
        switch (pCurrDescriptor->bDescriptorType)
        {
            case USB_INTERFACE_DESCRIPTOR_TYPE:
            {
                PUSB_INTERFACE_DESCRIPTOR pIfDescriptor = (PUSB_INTERFACE_DESCRIPTOR) pCurrDescriptor;
                currentInterfaceNumber = pIfDescriptor->bInterfaceNumber;
                break;
            }

            case USB_CS_INTERFACE_TYPE:
            {
                // Only process descriptors for OUR control interface
                if (currentInterfaceNumber != controlInterfaceNumber)
                {
                    break;
                }

                PUSB_CDC_CS_FUNCTIONAL_DESCRIPTOR pCsFuncDescriptor =
                    (PUSB_CDC_CS_FUNCTIONAL_DESCRIPTOR)pCurrDescriptor;

                switch (pCsFuncDescriptor->bDescriptorSubtype)
                {
                    case USB_CS_NCM_FUNCTIONAL_DESCR_TYPE:
                    {
                        if (pCsFuncDescriptor->bFunctionLength == sizeof(USB_NCM_CS_FUNCTIONAL_DESCRIPTOR))
                        {
                            pNcmFunctionalDescr = (PUSB_NCM_CS_FUNCTIONAL_DESCRIPTOR) pCsFuncDescriptor;
                        }
                        break;
                    }

                    case USB_CS_ECM_FUNCTIONAL_DESCR_TYPE:
                    {
                        if (pCsFuncDescriptor->bFunctionLength == sizeof(USB_ECM_CS_NET_FUNCTIONAL_DESCRIPTOR))
                        {
                            pEcmFunctionalDescr = (PUSB_ECM_CS_NET_FUNCTIONAL_DESCRIPTOR) pCsFuncDescriptor;
                        }
                        break;
                    }
                }
                break;
            }
        }

        currDescrptorOffset += pCurrDescriptor->bLength;
        pCurrDescriptor = (PUSB_COMMON_DESCRIPTOR)(((PUINT8)pCurrDescriptor) + pCurrDescriptor->bLength);
    }

    if (!pEcmFunctionalDescr)
    {
        DbgPrint("USBNCM: ERROR - No ECM functional descriptor found!\n");
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    // Query MTU
    m_MaxDatagramSize = min(pEcmFunctionalDescr->wMaxSegmentSize, MAX_HOST_MTU_SIZE);

    // Query MAC address
    WCHAR strMacAddress[12];
    USHORT strMacAddressLength = ARRAYSIZE(strMacAddress);

    status = WdfUsbTargetDeviceQueryString(
        m_WdfUsbTargetDevice,
        NULL,
        NULL,
        strMacAddress,
        &strMacAddressLength,
        pEcmFunctionalDescr->iMACAddress,
        0x0409);
    if (!NT_SUCCESS(status))
    {
        DbgPrint("USBNCM: Failed to get MAC address string 0x%08X\n", status);
        return status;
    }

    status = HexStringToBytes(strMacAddress, m_MacAddress, sizeof(m_MacAddress));
    if (!NT_SUCCESS(status))
    {
        DbgPrint("USBNCM: Failed to parse MAC address 0x%08X\n", status);
        return status;
    }

    // Get NTB parameters
    WDF_MEMORY_DESCRIPTOR memoryDescriptor;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
        &memoryDescriptor,
        &m_NtbParamters,
        sizeof(m_NtbParamters));

    status = RequestClassSpecificControlTransfer(
        USB_REQUEST_GET_NTB_PARAMETERS,
        BmRequestDeviceToHost,
        BmRequestToInterface,
        0,
        &memoryDescriptor);
    if (!NT_SUCCESS(status))
    {
        DbgPrint("USBNCM: GET_NTB_PARAMETERS failed 0x%08X\n", status);
        return status;
    }

    // NTB 16 must be supported
    if (!(m_NtbParamters.bmNtbFormatsSupported & 0x1))
    {
        DbgPrint("USBNCM: ERROR - NTB16 format not supported!\n");
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    // Use NTB 32 if supported
    if (m_NtbParamters.bmNtbFormatsSupported & 0x2)
    {
        m_Use32BitNtb = TRUE;
    }

    m_HostSelectedNtbInMaxSize = min(
        m_NtbParamters.dwNtbInMaxSize,
        MAX_HOST_NTB_SIZE);

    return STATUS_SUCCESS;
}

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::SelectSetting(
    void
)
{
    NTSTATUS status;
    
    PAGED_CODE();

    // 1. Data interface at Setting 0
    WDF_USB_INTERFACE_SELECT_SETTING_PARAMS settingParams;
    WDF_USB_INTERFACE_SELECT_SETTING_PARAMS_INIT_SETTING(&settingParams, 0);

    status = WdfUsbInterfaceSelectSetting(m_DataInterface, WDF_NO_OBJECT_ATTRIBUTES, &settingParams);
    if (!NT_SUCCESS(status))
    {
        DbgPrint("USBNCM: SelectSetting(0) FAILED 0x%08X\n", status);
        return status;
    }

    // 2. Config NTB
    if (m_Use32BitNtb)
    {
        status = RequestClassSpecificControlTransfer(
            USB_REQUEST_SET_NTB_FORMAT,
            BmRequestHostToDevice,
            BmRequestToInterface,
            1,
            nullptr);
        if (!NT_SUCCESS(status))
        {
            // Apple devices may not support this - continue anyway
            DbgPrint("USBNCM: SET_NTB_FORMAT failed 0x%08X (may be OK)\n", status);
        }
    }

    if (m_HostSelectedNtbInMaxSize < m_NtbParamters.dwNtbInMaxSize)
    {
        WDF_MEMORY_DESCRIPTOR memoryDescriptor;
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
            &memoryDescriptor,
            (PVOID)&m_HostSelectedNtbInMaxSize,
            sizeof(m_HostSelectedNtbInMaxSize));

        status = RequestClassSpecificControlTransfer(
            USB_REQUEST_SET_NTB_INPUT_SIZE,
            BmRequestHostToDevice,
            BmRequestToInterface,
            0,
            &memoryDescriptor);
        if (!NT_SUCCESS(status))
        {
            DbgPrint("USBNCM: SET_NTB_INPUT_SIZE failed 0x%08X (may be OK)\n", status);
        }
    }

    // 3. Data interface at Setting 1
    WDF_USB_INTERFACE_SELECT_SETTING_PARAMS_INIT_SETTING(&settingParams, 1);

    status = WdfUsbInterfaceSelectSetting(m_DataInterface, WDF_NO_OBJECT_ATTRIBUTES, &settingParams);
    if (!NT_SUCCESS(status))
    {
        DbgPrint("USBNCM: SelectSetting(1) FAILED 0x%08X\n", status);
        return status;
    }

    return STATUS_SUCCESS;
}

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::RetrieveInterruptPipe(
    void
)
{
    PAGED_CODE();

    // Apple devices don't have an interrupt endpoint - skip
    if (m_IsAppleDevice)
    {
        return STATUS_SUCCESS;
    }

    WDF_USB_PIPE_INFORMATION pipeInfo;
    WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);

    NCM_RETURN_NT_STATUS_IF_FALSE_MSG(
        WdfUsbInterfaceGetNumConfiguredPipes(m_ControlInterface) == 1,
        STATUS_DEVICE_HARDWARE_ERROR,
        "Bad NCM control interface");

    m_ControlInterruptPipe = WdfUsbInterfaceGetConfiguredPipe(
        m_ControlInterface,
        0,
        &pipeInfo);

    NCM_RETURN_NT_STATUS_IF_FALSE_MSG(
        pipeInfo.PipeType == WdfUsbPipeTypeInterrupt,
        STATUS_DEVICE_HARDWARE_ERROR,
        "Bad NCM control pipe type");

    WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(m_ControlInterruptPipe);
    m_ControlInterruptPipeMaxPacket = pipeInfo.MaximumPacketSize;

    WDF_USB_CONTINUOUS_READER_CONFIG readerConfig;
    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(
        &readerConfig,
        UsbNcmHostDevice::ControlInterruptPipeReadCompletetionRoutine,
        this,
        m_ControlInterruptPipeMaxPacket);

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfUsbTargetPipeConfigContinuousReader(
            m_ControlInterruptPipe,
            &readerConfig),
        "WdfUsbTargetPipeConfigContinuousReader failed for interrupt pipe");

    return STATUS_SUCCESS;
}


PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::RetrieveDataBulkPipes(
    void
)
{
    PAGED_CODE();

    NCM_RETURN_NT_STATUS_IF_FALSE_MSG(
        WdfUsbInterfaceGetNumConfiguredPipes(m_DataInterface) == 2,
        STATUS_DEVICE_HARDWARE_ERROR,
        "Bad NCM data interface");

    for (UCHAR pipeIndex = 0; pipeIndex < 2; pipeIndex++)
    {
        WDF_USB_PIPE_INFORMATION pipeInfo;
        WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);

        WDFUSBPIPE pipe = WdfUsbInterfaceGetConfiguredPipe(
            m_DataInterface,
            pipeIndex,
            &pipeInfo);

        NCM_RETURN_NT_STATUS_IF_FALSE_MSG(
            pipeInfo.PipeType == WdfUsbPipeTypeBulk,
            STATUS_DEVICE_HARDWARE_ERROR,
            "Bad NCM data pipe type");

        WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);

        if (WdfUsbTargetPipeIsInEndpoint(pipe))
        {
            m_DataBulkInPipe = pipe;
        }
        else if (WdfUsbTargetPipeIsOutEndpoint(pipe))
        {
            m_DataBulkOutPipeMaximumPacketSize = pipeInfo.MaximumPacketSize;
            m_DataBulkOutPipe = pipe;
        }
    }

    WDF_USB_CONTINUOUS_READER_CONFIG readerConfig;
    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(
        &readerConfig,
        UsbNcmHostDevice::DataBulkInPipeReadCompletetionRoutine,
        this,
        m_HostSelectedNtbInMaxSize);

    readerConfig.HeaderLength = 0;
    readerConfig.NumPendingReads = PENDING_BULK_IN_READS;

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfUsbTargetPipeConfigContinuousReader(
            m_DataBulkInPipe,
            &readerConfig),
        "WdfUsbTargetPipeConfigContinuousReader failed for bulkin pipe");

    return STATUS_SUCCESS;
}

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::EnterWorkingState(
    WDF_POWER_DEVICE_STATE previousState
)
{
    PAGED_CODE();

    // Placeholder instances don't need power management
    if (m_IsDataInterfaceOnly)
    {
        return STATUS_SUCCESS;
    }

    if (previousState != WdfPowerDeviceD3Final)
    {
        NCM_RETURN_IF_NOT_NT_SUCCESS(SelectSetting());
        NCM_RETURN_IF_NOT_NT_SUCCESS(RetrieveDataBulkPipes());
    }

    if (m_IsAppleDevice)
    {
        // Apple devices don't have status notifications - force link up with speed
        if (m_NcmAdapterCallbacks != nullptr)
        {
            m_NcmAdapterCallbacks->EvtUsbNcmAdapterSetLinkState(m_NetAdapter, TRUE);
            
            // Query actual USB connection speed and set link speed accordingly
            WDF_USB_DEVICE_INFORMATION usbInfo;
            WDF_USB_DEVICE_INFORMATION_INIT(&usbInfo);
            WdfUsbTargetDeviceRetrieveInformation(m_WdfUsbTargetDevice, &usbInfo);
            
            // WDF only tells us High Speed vs not-High Speed
            // If not High Speed on a modern Apple device, assume SuperSpeed
            ULONG32 linkSpeed;
            const char* speedStr;
            if (usbInfo.Traits & WDF_USB_DEVICE_TRAIT_AT_HIGH_SPEED)
            {
                // USB 2.0 High Speed = 480 Mbps
                linkSpeed = 480000000UL;
                speedStr = "480 Mbps (USB 2.0)";
            }
            else
            {
                // Not High Speed = likely SuperSpeed (USB 3.x)
                // Use 1 Gbps as practical network speed (ULONG32 max is ~4.3 Gbps)
                linkSpeed = 1000000000UL;
                speedStr = "1 Gbps (USB 3.x)";
            }
            DbgPrint("USBNCM: Link speed: %s\n", speedStr);
            m_NcmAdapterCallbacks->EvtUsbNcmAdapterSetLinkSpeed(m_NetAdapter, linkSpeed, linkSpeed);
        }
    }
    else
    {
        NCM_RETURN_IF_NOT_NT_SUCCESS(StartPipe(m_ControlInterruptPipe));
    }

    return STATUS_SUCCESS;
}

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::LeaveWorkingState(
    void
)
{
    PAGED_CODE();

    // Placeholder instances don't need power management
    if (m_IsDataInterfaceOnly)
    {
        return STATUS_SUCCESS;
    }

    if (!m_IsAppleDevice && m_ControlInterruptPipe != nullptr)
    {
        StopPipe(m_ControlInterruptPipe);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
UsbNcmHostDevice::ControlInterruptPipeReadCompletetionRoutine(
    WDFUSBPIPE,
    WDFMEMORY memory,
    size_t numBytesTransfered,
    WDFCONTEXT context
)
{
    UsbNcmHostDevice * ncmDevice = (UsbNcmHostDevice *)context;

    if (numBytesTransfered < sizeof(USB_CDC_NOTIFICATION))
    {
        return;
    }

    PUSB_CDC_NOTIFICATION cdcNotification =
        (PUSB_CDC_NOTIFICATION)WdfMemoryGetBuffer(memory, nullptr);

    switch (cdcNotification->bNotificationCode)
    {
        case USB_CDC_NOTIFICATION_NETWORK_CONNECTION:
        {
            ncmDevice->m_NcmAdapterCallbacks->EvtUsbNcmAdapterSetLinkState(
                ncmDevice->m_NetAdapter,
                !!cdcNotification->wValue);
            break;
        }
        case USB_CDC_NOTIFICATION_CONNECTION_SPEED_CHANGE:
        {
            PCDC_CONN_SPEED_CHANGE cdcSpeedChange =
                (PCDC_CONN_SPEED_CHANGE) cdcNotification;

            ncmDevice->m_NcmAdapterCallbacks->EvtUsbNcmAdapterSetLinkSpeed(
                ncmDevice->m_NetAdapter,
                cdcSpeedChange->USBitRate,
                cdcSpeedChange->DSBITRate);
            break;
        }
        default:
            break;
    }
}

_Use_decl_annotations_
VOID
UsbNcmHostDevice::DataBulkInPipeReadCompletetionRoutine(
    WDFUSBPIPE,
    WDFMEMORY memory,
    size_t numBytesTransferred,
    WDFCONTEXT context
)
{
    UsbNcmHostDevice* hostDevice = (UsbNcmHostDevice *)context;

    NT_FRE_ASSERT(hostDevice->m_HostSelectedNtbInMaxSize >= (UINT32)numBytesTransferred);

    hostDevice->m_NcmAdapterCallbacks->EvtUsbNcmAdapterNotifyReceive(
        hostDevice->m_NetAdapter,
        nullptr,
        0,
        memory,
        WDF_NO_HANDLE);
}

PAGEDX
_Use_decl_annotations_
void
UsbNcmHostDevice::StartReceive(
    WDFDEVICE usbNcmWdfDevice
)
{
    UsbNcmHostDevice* hostDevice = NcmGetHostDeviceFromHandle(usbNcmWdfDevice);
    if (hostDevice->m_DataBulkInPipe != nullptr)
    {
        (void) StartPipe(hostDevice->m_DataBulkInPipe);
    }
}

PAGEDX
_Use_decl_annotations_
void
UsbNcmHostDevice::StopReceive(
    WDFDEVICE usbNcmWdfDevice
)
{
    UsbNcmHostDevice* hostDevice = NcmGetHostDeviceFromHandle(usbNcmWdfDevice);
    if (hostDevice->m_DataBulkInPipe != nullptr)
    {
        StopPipe(hostDevice->m_DataBulkInPipe);
    }
}

PAGEDX
_Use_decl_annotations_
void
UsbNcmHostDevice::StartTransmit(
    WDFDEVICE usbNcmWdfDevice
)
{
    UsbNcmHostDevice* hostDevice = NcmGetHostDeviceFromHandle(usbNcmWdfDevice);
    if (hostDevice->m_DataBulkOutPipe != nullptr)
    {
        (void) StartPipe(hostDevice->m_DataBulkOutPipe);
    }
}

PAGEDX
_Use_decl_annotations_
void
UsbNcmHostDevice::StopTransmit(
    WDFDEVICE usbNcmWdfDevice
)
{
    UsbNcmHostDevice* hostDevice = NcmGetHostDeviceFromHandle(usbNcmWdfDevice);
    if (hostDevice->m_DataBulkOutPipe != nullptr)
    {
        StopPipe(hostDevice->m_DataBulkOutPipe);
    }
}

_Use_decl_annotations_
inline
void
UsbNcmHostDevice::TransmitFramesCompetion(
    WDFREQUEST,
    WDFIOTARGET target,
    PWDF_REQUEST_COMPLETION_PARAMS,
    WDFCONTEXT context
)
{
    UsbNcmHostDevice* hostDevice = NcmGetHostDeviceFromHandle(WdfIoTargetGetDevice(target));

    hostDevice->m_NcmAdapterCallbacks->EvtUsbNcmAdapterNotifyTransmitCompletion(
        hostDevice->m_NetAdapter,
        (TX_BUFFER_REQUEST *)context);
}

_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::TransmitFrames(
    WDFDEVICE usbNcmWdfDevice,
    TX_BUFFER_REQUEST * bufferRequest
)
{
    NTSTATUS status = STATUS_SUCCESS;
    UsbNcmHostDevice * hostDevice = NcmGetHostDeviceFromHandle(usbNcmWdfDevice);

    // Guard against NULL pipe
    if (hostDevice->m_DataBulkOutPipe == nullptr)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    NT_FRE_ASSERT(bufferRequest->TransferLength > 0);

    if (bufferRequest->TransferLength < bufferRequest->BufferLength &&
        bufferRequest->TransferLength % hostDevice->m_DataBulkOutPipeMaximumPacketSize == 0)
    {
        bufferRequest->Buffer[bufferRequest->TransferLength] = 0;
        bufferRequest->TransferLength++;
    }

    WdfRequestSetCompletionRoutine(
        bufferRequest->Request,
        UsbNcmHostDevice::TransmitFramesCompetion,
        bufferRequest);

    WDFMEMORY_OFFSET offset{ 0, bufferRequest->TransferLength };
    status = WdfUsbTargetPipeFormatRequestForWrite(
        hostDevice->m_DataBulkOutPipe,
        bufferRequest->Request,
        bufferRequest->BufferWdfMemory,
        &offset);

    if (NT_SUCCESS(status))
    {
        WDF_REQUEST_SEND_OPTIONS sendOptions = {};
        WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, WDF_REQUEST_SEND_OPTION_TIMEOUT);
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&sendOptions, WDF_REL_TIMEOUT_IN_SEC(5));

        if (!WdfRequestSend(
                bufferRequest->Request,
                WdfUsbTargetPipeGetIoTarget(hostDevice->m_DataBulkOutPipe), &sendOptions))
        {
            status = WdfRequestGetStatus(bufferRequest->Request);
        }
    }

    return status;
}
