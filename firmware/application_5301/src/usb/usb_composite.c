/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 akaInstruments */

#include "usb_composite.h"
#include "hpm_dfu_trigger.h"
#include "hpm_otp_drv.h"
#include "DAP.h"
#include "cdc_interface.h"
#include "scope_sampler.h"
#include "spi_bridge.h"
#include "spi_cdc.h"
#include "i2c_bridge.h"
#include "bus_periodic.h"
#include "adc_stream.h"

#define CMSIS_DAP_INTERFACE_SIZE (9 + 7 + 7 + 7)
#define CUSTOM_HID_LEN (9 + 9 + 7 + 7)
/* USB→SPI/QSPI 桥：vendor specific 接口 + 一对 bulk 端点（见 docs/usb-spi-bridge-plan.md）。
 * 只有引出 SPI2 排针的板子（HPM5301EVKLite，2026-09-30 从 SPI1 迁到 SPI2）才挂上去，
 * 其他板子的枚举结果保持原样。 */
#ifndef BOARD_HAS_SPI_BRIDGE
#define BOARD_HAS_SPI_BRIDGE (0)
#endif
#define SPI_BRIDGE_ENABLE (BOARD_HAS_SPI_BRIDGE)
#define SPI_BRIDGE_INTERFACE_SIZE (9 + 7 + 7)
#define DFU_RUNTIME_INTERFACE_SIZE (9 + 9)

#ifdef CONFIG_USB_HS
#define HIDRAW_INTERVAL 6 /* 4 ms */
#else
#define HIDRAW_INTERVAL 4
#endif

#define HID_CUSTOM_REPORT_DESC_SIZE 53

#define USBD_WINUSB_VENDOR_CODE 0x20
#define USBD_WEBUSB_VENDOR_CODE 0x21

#define USBD_WEBUSB_ENABLE 1
#define USBD_BULK_ENABLE 1
#define USBD_WINUSB_ENABLE 1
#define USBD_DFU_RUNTIME_ENABLE 1

/* WinUSB Microsoft OS 2.0 descriptor sizes */
#define WINUSB_DESCRIPTOR_SET_HEADER_SIZE 10
#define WINUSB_FUNCTION_SUBSET_HEADER_SIZE 8
#define WINUSB_FEATURE_COMPATIBLE_ID_SIZE 20

#define FUNCTION_SUBSET_LEN 160
#define DEVICE_INTERFACE_GUIDS_FEATURE_LEN 132

#define USBD_WINUSB_DESC_SET_LEN (WINUSB_DESCRIPTOR_SET_HEADER_SIZE +        \
                                  USBD_WEBUSB_ENABLE * FUNCTION_SUBSET_LEN + \
                                  USBD_BULK_ENABLE * FUNCTION_SUBSET_LEN +   \
                                  SPI_BRIDGE_ENABLE * FUNCTION_SUBSET_LEN +  \
                                  USBD_DFU_RUNTIME_ENABLE * FUNCTION_SUBSET_LEN)

#define USBD_NUM_DEV_CAPABILITIES (USBD_WEBUSB_ENABLE + USBD_WINUSB_ENABLE)

/* CherryUSB serves the MS OS 2.0 descriptor set through the shared EP0 request
 * buffer and STALLs the request when it does not fit ("Request buffer too
 * small", usbd_core.c). A stalled WCID request makes Windows fail the whole
 * composite device (Code 10) - no HID, no bulk, nothing. Keep the two numbers
 * tied together at compile time. */
#if (USBD_WINUSB_DESC_SET_LEN > CONFIG_USBDEV_REQUEST_BUFFER_LEN)
#error "MS OS 2.0 descriptor set does not fit into CONFIG_USBDEV_REQUEST_BUFFER_LEN (see src/usb/usb_config.h)"
#endif

#define USBD_WEBUSB_DESC_LEN 24
#define USBD_WINUSB_DESC_LEN 28

#define USBD_BOS_WTOTALLENGTH (0x05 +                                      \
                               USBD_WEBUSB_DESC_LEN * USBD_WEBUSB_ENABLE + \
                               USBD_WINUSB_DESC_LEN * USBD_WINUSB_ENABLE)

#define USB_CONFIG_SIZE (9 +                                                    \
                         CMSIS_DAP_INTERFACE_SIZE + CDC_ACM_DESCRIPTOR_LEN +    \
                         CONFIG_CHERRYDAP_USE_CUSTOM_HID * CUSTOM_HID_LEN +     \
                         SPI_BRIDGE_ENABLE * SPI_BRIDGE_INTERFACE_SIZE +        \
                         USBD_WEBUSB_ENABLE * 9 +                               \
                         USBD_DFU_RUNTIME_ENABLE * DFU_RUNTIME_INTERFACE_SIZE + \
                         CONFIG_CHERRYDAP_USE_MSC * MSC_DESCRIPTOR_LEN)

#define INTF_NUM (1 + 2 + CONFIG_CHERRYDAP_USE_CUSTOM_HID + SPI_BRIDGE_ENABLE + USBD_WEBUSB_ENABLE + USBD_DFU_RUNTIME_ENABLE + CONFIG_CHERRYDAP_USE_MSC)
#define HID_INTF_NUM (2 + CONFIG_CHERRYDAP_USE_CUSTOM_HID)
#define SPI_INTF_NUM (HID_INTF_NUM + CONFIG_CHERRYDAP_USE_CUSTOM_HID)
#define MSC_INTF_NUM (SPI_INTF_NUM + SPI_BRIDGE_ENABLE)
#define WEBUSB_INTF_NUM (MSC_INTF_NUM + CONFIG_CHERRYDAP_USE_MSC)
#define DFU_INTF_NUM (WEBUSB_INTF_NUM + 1)

#define DFU_INTF_STRING_INDEX 0x05
#define WEB_INTF_STRING_INDEX 0x04
#define MSC_INTF_STRING_INDEX 0x00
#define CDC_INTF_STRING_INDEX 0x00


// clang-format off
__ALIGN_BEGIN const uint8_t USBD_WinUSBDescriptorSetDescriptor[] = {
    WBVAL(WINUSB_DESCRIPTOR_SET_HEADER_SIZE), /* wLength */
    WBVAL(WINUSB_SET_HEADER_DESCRIPTOR_TYPE), /* wDescriptorType */
    0x00, 0x00, 0x03, 0x06, /* >= Win 8.1 */  /* dwWindowsVersion*/
    WBVAL(USBD_WINUSB_DESC_SET_LEN),          /* wDescriptorSetTotalLength */
#if (USBD_WEBUSB_ENABLE)
    WBVAL(WINUSB_FUNCTION_SUBSET_HEADER_SIZE), // wLength
    WBVAL(WINUSB_SUBSET_HEADER_FUNCTION_TYPE), // wDescriptorType
    WEBUSB_INTF_NUM,                           // bFirstInterface USBD_WINUSB_IF_NUM
    0,                                         // bReserved
    WBVAL(FUNCTION_SUBSET_LEN),                // wSubsetLength
    WBVAL(WINUSB_FEATURE_COMPATIBLE_ID_SIZE),  // wLength
    WBVAL(WINUSB_FEATURE_COMPATIBLE_ID_TYPE),  // wDescriptorType
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0,        // CompatibleId
    0, 0, 0, 0, 0, 0, 0, 0,                    // SubCompatibleId
    WBVAL(DEVICE_INTERFACE_GUIDS_FEATURE_LEN), // wLength
    WBVAL(WINUSB_FEATURE_REG_PROPERTY_TYPE),   // wDescriptorType
    WBVAL(WINUSB_PROP_DATA_TYPE_REG_MULTI_SZ), // wPropertyDataType
    WBVAL(42),                                 // wPropertyNameLength
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0,
    'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0,
    'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
    WBVAL(80), // wPropertyDataLength
    '{', 0,
    '9', 0, '2', 0, 'C', 0, 'E', 0, '6', 0, '4', 0, '6', 0, '2', 0, '-', 0,
    '9', 0, 'C', 0, '7', 0, '7', 0, '-', 0,
    '4', 0, '6', 0, 'F', 0, 'E', 0, '-', 0,
    '9', 0, '3', 0, '3', 0, 'B', 0, '-',
    0, '3', 0, '1', 0, 'C', 0, 'B', 0, '9', 0, 'C', 0, '5', 0, 'A', 0, 'A', 0, '3', 0, 'B', 0, '9', 0,
    '}', 0, 0, 0, 0, 0,
#endif
#if USBD_BULK_ENABLE
    WBVAL(WINUSB_FUNCTION_SUBSET_HEADER_SIZE), /* wLength */
    WBVAL(WINUSB_SUBSET_HEADER_FUNCTION_TYPE), /* wDescriptorType */
    0,                                         /* bFirstInterface USBD_BULK_IF_NUM*/
    0,                                         /* bReserved */
    WBVAL(FUNCTION_SUBSET_LEN),                /* wSubsetLength */
    WBVAL(WINUSB_FEATURE_COMPATIBLE_ID_SIZE),  /* wLength */
    WBVAL(WINUSB_FEATURE_COMPATIBLE_ID_TYPE),  /* wDescriptorType */
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0,        /* CompatibleId*/
    0, 0, 0, 0, 0, 0, 0, 0,                    /* SubCompatibleId*/
    WBVAL(DEVICE_INTERFACE_GUIDS_FEATURE_LEN), /* wLength */
    WBVAL(WINUSB_FEATURE_REG_PROPERTY_TYPE),   /* wDescriptorType */
    WBVAL(WINUSB_PROP_DATA_TYPE_REG_MULTI_SZ), /* wPropertyDataType */
    WBVAL(42),                                 /* wPropertyNameLength */
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0,
    'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0,
    'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
    WBVAL(80), /* wPropertyDataLength */
    '{', 0,
    'C', 0, 'D', 0, 'B', 0, '3', 0, 'B', 0, '5', 0, 'A', 0, 'D', 0, '-', 0,
    '2', 0, '9', 0, '3', 0, 'B', 0, '-', 0,
    '4', 0, '6', 0, '6', 0, '3', 0, '-', 0,
    'A', 0, 'A', 0, '3', 0, '6', 0, '-',
    0, '1', 0, 'A', 0, 'A', 0, 'E', 0, '4', 0, '6', 0, '4', 0, '6', 0, '3', 0, '7', 0, '7', 0, '6', 0,
    '}', 0, 0, 0, 0, 0,
#endif
#if SPI_BRIDGE_ENABLE
    /* WinUSB function subset for the USB->SPI bridge interface so that Windows
     * binds WinUSB to it and libusb / WebUSB can claim it. Without this subset
     * the interface enumerates fine but cannot be opened (libusb NOT_SUPPORTED),
     * which is exactly what the first on-board P1 run hit. */
    WBVAL(WINUSB_FUNCTION_SUBSET_HEADER_SIZE), /* wLength */
    WBVAL(WINUSB_SUBSET_HEADER_FUNCTION_TYPE), /* wDescriptorType */
    SPI_INTF_NUM,                              /* bFirstInterface */
    0,                                         /* bReserved */
    WBVAL(FUNCTION_SUBSET_LEN),                /* wSubsetLength */
    WBVAL(WINUSB_FEATURE_COMPATIBLE_ID_SIZE),  /* wLength */
    WBVAL(WINUSB_FEATURE_COMPATIBLE_ID_TYPE),  /* wDescriptorType */
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0,        /* CompatibleId*/
    0, 0, 0, 0, 0, 0, 0, 0,                    /* SubCompatibleId*/
    WBVAL(DEVICE_INTERFACE_GUIDS_FEATURE_LEN), /* wLength */
    WBVAL(WINUSB_FEATURE_REG_PROPERTY_TYPE),   /* wDescriptorType */
    WBVAL(WINUSB_PROP_DATA_TYPE_REG_MULTI_SZ), /* wPropertyDataType */
    WBVAL(42),                                 /* wPropertyNameLength */
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0,
    'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0,
    'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
    WBVAL(80), /* wPropertyDataLength */
    '{', 0,
    '3', 0, 'E', 0, '7', 0, 'B', 0, '1', 0, 'C', 0, '4', 0, '8', 0, '-', 0,
    '9', 0, 'D', 0, '2', 0, 'A', 0, '-', 0,
    '4', 0, 'F', 0, '6', 0, '1', 0, '-', 0,
    'B', 0, '5', 0, 'E', 0, '8', 0, '-', 0,
    '7', 0, 'C', 0, '0', 0, '4', 0, 'A', 0, '9', 0, 'D', 0, '3', 0, 'F', 0, '2', 0, '1', 0, '0', 0,
    '}', 0, 0, 0, 0, 0,
#endif
#if USBD_DFU_RUNTIME_ENABLE
    /* WinUSB function subset for the DFU runtime interface so that Windows
     * binds WinUSB to it and libusb (dfu-util) can claim the interface. */
    WBVAL(WINUSB_FUNCTION_SUBSET_HEADER_SIZE), /* wLength */
    WBVAL(WINUSB_SUBSET_HEADER_FUNCTION_TYPE), /* wDescriptorType */
    DFU_INTF_NUM,                              /* bFirstInterface */
    0,                                         /* bReserved */
    WBVAL(FUNCTION_SUBSET_LEN),                /* wSubsetLength */
    WBVAL(WINUSB_FEATURE_COMPATIBLE_ID_SIZE),  /* wLength */
    WBVAL(WINUSB_FEATURE_COMPATIBLE_ID_TYPE),  /* wDescriptorType */
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0,        /* CompatibleId*/
    0, 0, 0, 0, 0, 0, 0, 0,                    /* SubCompatibleId*/
    WBVAL(DEVICE_INTERFACE_GUIDS_FEATURE_LEN), /* wLength */
    WBVAL(WINUSB_FEATURE_REG_PROPERTY_TYPE),   /* wDescriptorType */
    WBVAL(WINUSB_PROP_DATA_TYPE_REG_MULTI_SZ), /* wPropertyDataType */
    WBVAL(42),                                 /* wPropertyNameLength */
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0,
    'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0,
    'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
    WBVAL(80), /* wPropertyDataLength */
    '{', 0,
    '8', 0, 'F', 0, '3', 0, 'A', 0, '6', 0, 'C', 0, '1', 0, 'D', 0, '-', 0,
    '4', 0, 'B', 0, '2', 0, '7', 0, '-', 0,
    '4', 0, 'E', 0, '9', 0, 'A', 0, '-', 0,
    'A', 0, 'C', 0, '1', 0, '5', 0, '-',
    0, '7', 0, 'D', 0, '2', 0, 'E', 0, '0', 0, 'B', 0, '6', 0, '9', 0, 'F', 0, '5', 0, 'C', 0, '3', 0,
    '}', 0, 0, 0, 0, 0
#endif
};
// clang-format on

// clang-format off
__ALIGN_BEGIN const uint8_t USBD_BinaryObjectStoreDescriptor[] = {
    0x05,                         /* bLength */
    0x0f,                         /* bDescriptorType */
    WBVAL(USBD_BOS_WTOTALLENGTH), /* wTotalLength */
    USBD_NUM_DEV_CAPABILITIES,    /* bNumDeviceCaps */
#if (USBD_WEBUSB_ENABLE)
    USBD_WEBUSB_DESC_LEN,           /* bLength */
    0x10,                           /* bDescriptorType */
    USB_DEVICE_CAPABILITY_PLATFORM, /* bDevCapabilityType */
    0x00,                           /* bReserved */
    0x38, 0xB6, 0x08, 0x34,         /* PlatformCapabilityUUID */
    0xA9, 0x09, 0xA0, 0x47,
    0x8B, 0xFD, 0xA0, 0x76,
    0x88, 0x15, 0xB6, 0x65,
    WBVAL(0x0100), /* 1.00 */ /* bcdVersion */
    USBD_WEBUSB_VENDOR_CODE,  /* bVendorCode */
    1,                        /* iLandingPage */
#endif
#if (USBD_WINUSB_ENABLE)
    USBD_WINUSB_DESC_LEN,           /* bLength */
    0x10,                           /* bDescriptorType */
    USB_DEVICE_CAPABILITY_PLATFORM, /* bDevCapabilityType */
    0x00,                           /* bReserved */
    0xDF, 0x60, 0xDD, 0xD8,         /* PlatformCapabilityUUID */
    0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D,
    0x9E, 0x64, 0x8A, 0x9F,
    0x00, 0x00, 0x03, 0x06, /* >= Win 8.1 */ /* dwWindowsVersion*/
    WBVAL(USBD_WINUSB_DESC_SET_LEN),         /* wDescriptorSetTotalLength */
    USBD_WINUSB_VENDOR_CODE,                 /* bVendorCode */
    0,                                       /* bAltEnumCode */
#endif
};
// clang-format on
/* WebUSB landing page (the URL Chrome's device notification links to).
 * bLength = strlen(url) + 3 (bLength + bDescriptorType + bScheme). */
static const struct
{
    uint8_t len;
    uint8_t type;
    uint8_t scheme;
    char url[sizeof("minichao9901.github.io/web-serial-rtt-tools") - 1];
} USBD_WebUSBURLDescriptor = {
    sizeof("minichao9901.github.io/web-serial-rtt-tools") - 1 + 3,
    WEBUSB_URL_TYPE,
    WEBUSB_URL_SCHEME_HTTPS,
    "minichao9901.github.io/web-serial-rtt-tools"};

#define URL_DESCRIPTOR_LENGTH sizeof(USBD_WebUSBURLDescriptor)

#if CONFIG_CHERRYDAP_USE_CUSTOM_HID
// clang-format off
#define HID_DESC()                                                                                                                   \
    /************** Descriptor of Custom interface *****************/                                                                \
    0x09,                                           /* bLength: Interface Descriptor size */                                         \
    USB_DESCRIPTOR_TYPE_INTERFACE,                  /* bDescriptorType: Interface descriptor type */                                 \
    HID_INTF_NUM,                                   /* bInterfaceNumber: Number of Interface */                                      \
    0x00,                                           /* bAlternateSetting: Alternate setting */                                       \
    0x02,                                           /* bNumEndpoints */                                                              \
    0x03,                                           /* bInterfaceClass: HID */                                                       \
    0x01,                                           /* bInterfaceSubClass : 1=BOOT, 0=no boot */                                     \
    0x00,                                           /* nInterfaceProtocol : 0=none, 1=keyboard, 2=mouse */                           \
    0, /* iInterface: Index of string descriptor */ /******************** Descriptor of Custom HID ********************/             \
    0x09,                                           /* bLength: HID Descriptor size */                                               \
    HID_DESCRIPTOR_TYPE_HID,                        /* bDescriptorType: HID */                                                       \
    0x11,                                           /* bcdHID: HID Class Spec release number */                                      \
    0x01,                                                                                                                            \
    0x00,                                              /* bCountryCode: Hardware target country */                                   \
    0x01,                                              /* bNumDescriptors: Number of HID class descriptors to follow */              \
    0x22,                                              /* bDescriptorType */                                                         \
    HID_CUSTOM_REPORT_DESC_SIZE,                       /* wItemLength: Total length of Report descriptor */                          \
    0x00,                                              /******************** Descriptor of Custom in endpoint ********************/  \
    0x07,                                              /* bLength: Endpoint Descriptor size */                                       \
    USB_DESCRIPTOR_TYPE_ENDPOINT,                      /* bDescriptorType: */                                                        \
    HID_IN_EP,                                         /* bEndpointAddress: Endpoint Address (IN) */                                 \
    0x03,                                              /* bmAttributes: Interrupt endpoint */                                        \
    WBVAL(HID_PACKET_SIZE),                            /* wMaxPacketSize: 4 Byte max */                                              \
    HIDRAW_INTERVAL, /* bInterval: Polling Interval */ /******************** Descriptor of Custom out endpoint ********************/ \
    0x07,                                              /* bLength: Endpoint Descriptor size */                                       \
    USB_DESCRIPTOR_TYPE_ENDPOINT,                      /* bDescriptorType: */                                                        \
    HID_OUT_EP,                                        /* bEndpointAddress: Endpoint Address (IN) */                                 \
    0x03,                                              /* bmAttributes: Interrupt endpoint */                                        \
    WBVAL(HID_PACKET_SIZE),                            /* wMaxPacketSize: 4 Byte max */                                              \
    HIDRAW_INTERVAL                                    /* bInterval: Polling Interval */
// clang-format on
#endif

/* USB→SPI/QSPI 桥：vendor specific 接口（0xFF）+ 一对 512 B bulk 端点。
 * 协议见 src/spi_bridge/spi_bridge_proto.h 与 docs/usb-spi-bridge-plan.md §4。 */
// clang-format off
#define SPI_BRIDGE_DESC()                                                           \
    /************** Descriptor of SPI bridge interface *****************/           \
    0x09,                                    /* bLength: Interface Descriptor size */\
    USB_DESCRIPTOR_TYPE_INTERFACE,           /* bDescriptorType */                  \
    SPI_INTF_NUM,                            /* bInterfaceNumber */                 \
    0x00,                                    /* bAlternateSetting */                \
    0x02,                                    /* bNumEndpoints */                    \
    0xFF,                                    /* bInterfaceClass: vendor specific */ \
    0x00,                                    /* bInterfaceSubClass */               \
    0x00,                                    /* nInterfaceProtocol */               \
    0x00,                                    /* iInterface */                       \
    /************** bulk OUT (host -> probe) *****************/                     \
    0x07,                                    /* bLength: Endpoint Descriptor size */\
    USB_DESCRIPTOR_TYPE_ENDPOINT,            /* bDescriptorType */                  \
    SPI_OUT_EP,                              /* bEndpointAddress (OUT) */           \
    USB_ENDPOINT_TYPE_BULK,                  /* bmAttributes */                     \
    WBVAL(DAP_PACKET_SIZE),                  /* wMaxPacketSize */                   \
    0x00,                                    /* bInterval */                        \
    /************** bulk IN (probe -> host) *****************/                      \
    0x07,                                    /* bLength: Endpoint Descriptor size */\
    USB_DESCRIPTOR_TYPE_ENDPOINT,            /* bDescriptorType */                  \
    SPI_IN_EP,                               /* bEndpointAddress (IN) */            \
    USB_ENDPOINT_TYPE_BULK,                  /* bmAttributes */                     \
    WBVAL(DAP_PACKET_SIZE),                  /* wMaxPacketSize */                   \
    0x00                                     /* bInterval */
// clang-format on

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_1, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID, 0x0100, 0x01),
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, INTF_NUM, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    /* Interface 0 */
    USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x03, 0xFF, 0x00, 0x00, 0x02),
    /* Endpoint OUT 2 */
    USB_ENDPOINT_DESCRIPTOR_INIT(DAP_OUT_EP, USB_ENDPOINT_TYPE_BULK, DAP_PACKET_SIZE, 0x00),
    /* Endpoint IN 1 */
    USB_ENDPOINT_DESCRIPTOR_INIT(DAP_IN_EP, USB_ENDPOINT_TYPE_BULK, DAP_PACKET_SIZE, 0x00),
    /* Endpoint IN 2 */
    USB_ENDPOINT_DESCRIPTOR_INIT(SWO_IN_EP, USB_ENDPOINT_TYPE_BULK, DAP_PACKET_SIZE, 0x00),
    CDC_ACM_DESCRIPTOR_INIT(0x01, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, DAP_PACKET_SIZE, CDC_INTF_STRING_INDEX),
#if CONFIG_CHERRYDAP_USE_CUSTOM_HID
    HID_DESC(),
#endif
#if SPI_BRIDGE_ENABLE
    SPI_BRIDGE_DESC(),
#endif
#if CONFIG_CHERRYDAP_USE_MSC
    MSC_DESCRIPTOR_INIT(MSC_INTF_NUM, MSC_OUT_EP, MSC_IN_EP, DAP_PACKET_SIZE, MSC_INTF_STRING_INDEX),
#endif
#if USBD_WEBUSB_ENABLE
    USB_INTERFACE_DESCRIPTOR_INIT(WEBUSB_INTF_NUM, 0x00, 0x00, 0xff, 0x00, 0x00, WEB_INTF_STRING_INDEX),
#endif
#if USBD_DFU_RUNTIME_ENABLE
    /* DFU runtime interface (no endpoints, uses EP0 only) */
    USB_INTERFACE_DESCRIPTOR_INIT(DFU_INTF_NUM, 0x00, 0x00, 0xFE, 0x01, 0x01, DFU_INTF_STRING_INDEX),
    /* DFU Functional Descriptor */
    0x09,
    0x21, /* bLength, bDescriptorType = DFU_FUNCTIONAL */
    0x0B, /* bmAttributes: bitCanDnload|bitCanUpload|bitWillDetach */
    0xFF,
    0x00, /* wDetachTimeOut = 255ms */
    0x00,
    0x10, /* wTransferSize = 4096 */
    0x1A,
    0x01, /* bcdDFU = 1.1a */
#endif
};

static const uint8_t other_speed_config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, INTF_NUM, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    /* Interface 0 */
    USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x03, 0xFF, 0x00, 0x00, 0x02),
    /* Endpoint OUT 2 */
    USB_ENDPOINT_DESCRIPTOR_INIT(DAP_OUT_EP, USB_ENDPOINT_TYPE_BULK, DAP_PACKET_SIZE, 0x00),
    /* Endpoint IN 1 */
    USB_ENDPOINT_DESCRIPTOR_INIT(DAP_IN_EP, USB_ENDPOINT_TYPE_BULK, DAP_PACKET_SIZE, 0x00),
    /* Endpoint IN 2 */
    USB_ENDPOINT_DESCRIPTOR_INIT(SWO_IN_EP, USB_ENDPOINT_TYPE_BULK, DAP_PACKET_SIZE, 0x00),
    CDC_ACM_DESCRIPTOR_INIT(0x01, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, DAP_PACKET_SIZE, CDC_INTF_STRING_INDEX),
#if CONFIG_CHERRYDAP_USE_CUSTOM_HID
    HID_DESC(),
#endif
#if SPI_BRIDGE_ENABLE
    SPI_BRIDGE_DESC(),
#endif
#if CONFIG_CHERRYDAP_USE_MSC
    MSC_DESCRIPTOR_INIT(0x04, MSC_OUT_EP, MSC_IN_EP, DAP_PACKET_SIZE, MSC_INTF_STRING_INDEX),
#endif
#if USBD_WEBUSB_ENABLE
    USB_INTERFACE_DESCRIPTOR_INIT(WEBUSB_INTF_NUM, 0x00, 0x00, 0xff, 0x00, 0x00, WEB_INTF_STRING_INDEX),
#endif
#if USBD_DFU_RUNTIME_ENABLE
    /* DFU runtime interface (no endpoints, uses EP0 only) */
    USB_INTERFACE_DESCRIPTOR_INIT(DFU_INTF_NUM, 0x00, 0x00, 0xFE, 0x01, 0x01, DFU_INTF_STRING_INDEX),
    /* DFU Functional Descriptor */
    0x09,
    0x21, /* bLength, bDescriptorType = DFU_FUNCTIONAL */
    0x0B, /* bmAttributes: bitCanDnload|bitCanUpload|bitWillDetach */
    0xFF,
    0x00, /* wDetachTimeOut = 255ms */
    0x00,
    0x10, /* wTransferSize = 4096 */
    0x1A,
    0x01, /* bcdDFU = 1.1a */
#endif
};

#if CONFIG_CHERRYDAP_USE_CUSTOM_HID
// clang-format off
/*!< custom hid report descriptor */
const uint8_t hid_custom_report_desc[HID_CUSTOM_REPORT_DESC_SIZE] = {
    /* USER CODE BEGIN 0 */
    0x06, 0x00, 0xff, /* USAGE_PAGE (Vendor Defined Page 1) */
    0x09, 0x01,       /* USAGE (Vendor Usage 1) */
    0xa1, 0x01,       /* COLLECTION (Application) */
    0x85, 0x02,       /*   REPORT ID (0x02) */
    0x09, 0x02,       /*   USAGE (Vendor Usage 1) */
    0x15, 0x00,       /*   LOGICAL_MINIMUM (0) */
    0x25, 0xff,       /*LOGICAL_MAXIMUM (255) */
    0x75, 0x08,       /*   REPORT_SIZE (8) */
    0x96, 0x3f, 0x00, /*   REPORT_COUNT (63) */
    0x81, 0x02,       /*   INPUT (Data,Var,Abs) */
    /* <___________________________________________________> */
    0x85, 0x01,       /*   REPORT ID (0x01) */
    0x09, 0x03,       /*   USAGE (Vendor Usage 1) */
    0x15, 0x00,       /*   LOGICAL_MINIMUM (0) */
    0x25, 0xff,       /*   LOGICAL_MAXIMUM (255) */
    0x75, 0x08,       /*   REPORT_SIZE (8) */
    0x96, 0x3f, 0x00, /*   REPORT_COUNT (63) */
    0x91, 0x02,       /*   OUTPUT (Data,Var,Abs) */

    /* <___________________________________________________> */
    0x85, 0x03,       /*   REPORT ID (0x03) */
    0x09, 0x04,       /*   USAGE (Vendor Usage 1) */
    0x15, 0x00,       /*   LOGICAL_MINIMUM (0) */
    0x25, 0xff,       /*   LOGICAL_MAXIMUM (255) */
    0x75, 0x08,       /*   REPORT_SIZE (8) */
    0x96, 0x3f, 0x00, /*   REPORT_COUNT (63) */
    0xb1, 0x02,       /*   FEATURE (Data,Var,Abs) */
    /* USER CODE END 0 */
    0xC0 /*     END_COLLECTION	             */
};
// clang-format on
#endif

char serial_number_dynamic[33] = {0}; // Dynamic serial number

char *string_descriptors[] = {
    (char[]){0x09, 0x04},   /* Langid */
    "ARM",                  /* Manufacturer */
    "akaLinkPro CMSIS-DAP", /* Product */
    "Serial Number",        /* Serial Number */
    "akaLinkPro WebUSB",
    "akaLinkPro DFU Runtime",
};

static const uint8_t device_quality_descriptor[] = {
    USB_DEVICE_QUALIFIER_DESCRIPTOR_INIT(USB_2_1, 0x00, 0x00, 0x00, 0x01),
};

__WEAK const uint8_t *device_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return device_descriptor;
}

__WEAK const uint8_t *config_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return config_descriptor;
}

__WEAK const uint8_t *device_quality_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return device_quality_descriptor;
}

__WEAK const uint8_t *other_speed_config_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return other_speed_config_descriptor;
}

__WEAK const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    (void)speed;

    if (index == 3)
    {
        return serial_number_dynamic;
    }

    if (index >= (sizeof(string_descriptors) / sizeof(char *)))
    {
        return NULL;
    }
    return string_descriptors[index];
}

static volatile uint16_t USB_RequestIndexI = 0; // Request  Index In
static volatile uint16_t USB_RequestIndexO = 0; // Request  Index Out
static volatile uint16_t USB_RequestCountI = 0; // Request  Count In
static volatile uint16_t USB_RequestCountO = 0; // Request  Count Out
static volatile uint8_t USB_RequestIdle = 1;    // Request  Idle  Flag

static volatile uint16_t USB_ResponseIndexI = 0; // Response Index In
static volatile uint16_t USB_ResponseIndexO = 0; // Response Index Out
static volatile uint16_t USB_ResponseCountI = 0; // Response Count In
static volatile uint16_t USB_ResponseCountO = 0; // Response Count Out
static volatile uint8_t USB_ResponseIdle = 1;    // Response Idle  Flag

/* USB 复位代数：每次 USBD_EVENT_RESET +1（总线复位时在飞传输被硬件作废、回调不会再来）。
 *
 * 为什么需要它：`chry_dap_handle()` 的排空循环靠 `CountI != CountO` 判断还有没有命令。
 * 若总线复位**正好插在"取出命令执行"和"记账"之间**，复位中断会把四个计数清零，而主循环
 * 随后照样把 CountO 加一 —— 于是 CountO = CountI + 1。这个差再也不会自己归位
 * （CountO 每圈 +1，CountI 只跟着主机的新命令走），循环会一路空转、把请求缓冲里的
 * **陈旧命令**反复执行（其中可能有对目标的写），直到 16 位计数回绕（65536 次）才停。
 * 有了代数，主循环在记账前比一次：变了就整轮作废，让计数停在 ISR 清零后的相等状态。 */
static volatile uint16_t USB_ResetGen = 0;

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t USB_Request[DAP_PACKET_COUNT][DAP_XFER_SIZE];  // Request  Buffer
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t USB_Response[DAP_PACKET_COUNT][DAP_XFER_SIZE]; // Response Buffer
static uint16_t USB_RespSize[DAP_PACKET_COUNT];                                                        // Response Size

volatile struct cdc_line_coding g_cdc_lincoding;
volatile uint8_t config_uart = 0;
volatile uint8_t config_uart_transfer = 0;
static volatile uint8_t cdc_configured = 0U; /* reset ISR / main-loop setup */

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t uartrx_ringbuffer[CONFIG_UARTRX_RINGBUF_SIZE];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t usbrx_ringbuffer[CONFIG_USBRX_RINGBUF_SIZE];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t usb_tmpbuffer[DAP_PACKET_SIZE];

static volatile uint8_t usbrx_idle_flag = 0;
static volatile uint8_t usbtx_idle_flag = 0;
static volatile uint8_t uarttx_idle_flag = 0;

/* 主循环级 CDC/串口桥总开关（默认开）。见 chry_dap_usb2uart_set_enabled()。 */
volatile uint8_t usb2uart_bridge_enabled = 1U;

USB_NOCACHE_RAM_SECTION chry_ringbuffer_t g_uartrx;
USB_NOCACHE_RAM_SECTION chry_ringbuffer_t g_usbrx;

void usbd_event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;
    switch (event)
    {
    case USBD_EVENT_RESET:
        spi_cdc_usb_reset();
        /* 先记代数：主循环据此判断"我手上这条命令是不是已经被复位作废了" */
        USB_ResetGen++;
        usbrx_idle_flag = 0;
        usbtx_idle_flag = 0;
        uarttx_idle_flag = 0;
        config_uart_transfer = 0;
        cdc_configured = 0U;
        /* DAP 的队列索引/计数必须一起复位：下面的 CONFIGURED 固定从 USB_Request[0]
         * 重新武装，索引不复位的话，dap_out_callback 会拿旧 IndexI 去判 TransferAbort
         * （读的是旧缓冲），而数据其实落在 [0] —— 命令流错乱，一直到下一次复位。
         * 计数清零同时也让主循环里那个 while (CountI != CountO) 变空转。 */
        USB_RequestIndexI = 0U;
        USB_RequestIndexO = 0U;
        USB_RequestCountI = 0U;
        USB_RequestCountO = 0U;
        USB_ResponseIndexI = 0U;
        USB_ResponseIndexO = 0U;
        USB_ResponseCountI = 0U;
        USB_ResponseCountO = 0U;
        USB_RequestIdle = 1U;
        USB_ResponseIdle = 1U;
        /* J-Scope 采样器的包缓冲账本同理：在飞的 bulk IN 0x83 传输全被复位作废，
         * 完成回调不会再来。这里只置标志，真正的清账放主循环做（不和推包抢状态）。 */
        scope_sampler_usb_reset();
        /* USB→SPI 桥的 OUT/IN 环同理：在飞的 0x0B/0x8B 传输作废，环归零 */
        spi_bridge_usb_reset();
        /* USB→I2C 桥没有自己的端点，只有"登记了还没做完的那次事务"要作废 */
        i2c_bridge_usb_reset();
        bus_periodic_reset();
        adc_stream_reset(0U);
        break;
    case USBD_EVENT_CONNECTED:
        break;
    case USBD_EVENT_DISCONNECTED:
        bus_periodic_reset();
        adc_stream_reset(0U);
        break;
    case USBD_EVENT_RESUME:
        break;
    case USBD_EVENT_SUSPEND:
        break;
    case USBD_EVENT_CONFIGURED:
        adc_stream_reset(1U);
        /* setup first out ep read transfer */
        USB_RequestIdle = 0U;
        usbd_ep_start_read(0, DAP_OUT_EP, USB_Request[0], DAP_XFER_SIZE);
        usbd_ep_start_read(0, CDC_OUT_EP, usb_tmpbuffer, DAP_PACKET_SIZE);
        /* USB→SPI 桥：武装它的 OUT 端点（未使能时内部会直接返回，主机侧收到 NAK） */
        spi_bridge_usb_ready();
        /* Re-arm the UART bridge as well: the reset above cleared
         * config_uart_transfer and a host that reuses its previous line coding
         * would otherwise never restart it. */
        if (g_cdc_lincoding.dwDTERate != 0U)
        {
            config_uart = 1;
        }
        break;
    case USBD_EVENT_SET_REMOTE_WAKEUP:
        break;
    case USBD_EVENT_CLR_REMOTE_WAKEUP:
        break;

    default:
        break;
    }
}

void dap_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    if (USB_Request[USB_RequestIndexI][0] == ID_DAP_TransferAbort) {
        DAP_Data.transfer_abort = 1U;
    } else {
        USB_RequestIndexI++;
        if (USB_RequestIndexI == DAP_PACKET_COUNT) {
            USB_RequestIndexI = 0U;
        }
        USB_RequestCountI++;
    }

    // Start reception of next request packet
    if ((uint16_t)(USB_RequestCountI - USB_RequestCountO) != DAP_PACKET_COUNT) {
        usbd_ep_start_read(0, DAP_OUT_EP, USB_Request[USB_RequestIndexI], DAP_XFER_SIZE);
    } else {
        USB_RequestIdle = 1U;
    }
}

void dap_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    if (USB_ResponseCountI != USB_ResponseCountO) {
        // Load data from response buffer to be sent back
        usbd_ep_start_write(0, DAP_IN_EP, USB_Response[USB_ResponseIndexO], USB_RespSize[USB_ResponseIndexO]);
        USB_ResponseIndexO++;
        if (USB_ResponseIndexO == DAP_PACKET_COUNT) {
            USB_ResponseIndexO = 0U;
        }
        USB_ResponseCountO++;
    } else {
        USB_ResponseIdle = 1U;
    }
}

void swo_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;
    /* 这条 bulk IN（0x83）原本挂给 SWO，但 SWO_STREAM=0 从来没写过它。
     * 现在给 J-Scope 采样器当数据面：一次 512 B 写完成 → 还回一个包缓冲。
     * 回调不带缓冲下标，采样器内部按提交顺序（FIFO）还。 */
    scope_sampler_tx_complete();
}

void usbd_cdc_acm_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    chry_ringbuffer_write(&g_usbrx, usb_tmpbuffer, nbytes);
    if (chry_ringbuffer_get_free(&g_usbrx) >= DAP_PACKET_SIZE)
    {
        usbd_ep_start_read(0, CDC_OUT_EP, usb_tmpbuffer, DAP_PACKET_SIZE);
    }
    else
    {
        usbrx_idle_flag = 1;
    }
}

void usbd_cdc_acm_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    uint32_t size;
    uint8_t *buffer;

    /* g_uartrx is produced by the UART RX flush ISRs, so keep the consumer
     * side in a critical section to avoid corrupting the ring indices. */
    uint32_t level = disable_global_irq(CSR_MSTATUS_MIE_MASK);

    chry_ringbuffer_linear_read_done(&g_uartrx, nbytes);

    /* 这里原来在「长度是 512 整数倍」时补发一个 ZLP。CDC 是纯字节流，主机不需要
     * 用它标记传输边界，而 RTT 桥是按 2048 字节往环里写的、线性窗经常正好是 512
     * 的倍数 ⇒ 几乎每次传输都白白多一个事务。去掉。
     * （如果真的需要 ZLP，也应该用 CDC_IN_EP 的 wMaxPacketSize 判断，而不是 DAP 的
     *   DAP_PACKET_SIZE —— 虽然两者在本工程里同为 512，但语义是 CDC 端点的属性。） */
    if (chry_ringbuffer_get_used(&g_uartrx))
    {
        buffer = chry_ringbuffer_linear_read_setup(&g_uartrx, &size);
        usbd_ep_start_write(0, CDC_IN_EP, buffer, size);
    }
    else
    {
        usbtx_idle_flag = 1;
    }

    restore_global_irq(level);
}

struct usbd_endpoint dap_out_ep = {
    .ep_addr = DAP_OUT_EP,
    .ep_cb = dap_out_callback};

struct usbd_endpoint dap_in_ep = {
    .ep_addr = DAP_IN_EP,
    .ep_cb = dap_in_callback};

struct usbd_endpoint swo_in_ep = {
    .ep_addr = SWO_IN_EP,
    .ep_cb = swo_in_callback};

struct usbd_endpoint cdc_out_ep = {
    .ep_addr = CDC_OUT_EP,
    .ep_cb = usbd_cdc_acm_bulk_out};

struct usbd_endpoint cdc_in_ep = {
    .ep_addr = CDC_IN_EP,
    .ep_cb = usbd_cdc_acm_bulk_in};

#if CONFIG_CHERRYDAP_USE_CUSTOM_HID
struct usbd_endpoint hid_custom_in_ep = {
    .ep_addr = HID_IN_EP,
    .ep_cb = usbd_hid_custom_in_callback,
};

struct usbd_endpoint hid_custom_out_ep = {
    .ep_addr = HID_OUT_EP,
    .ep_cb = usbd_hid_custom_out_callback,
};
#endif

/* ---- USB→SPI/QSPI 桥的两个回调：只做「投递/还槽」，真正的执行在主循环 ---- */

void spi_bridge_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    spi_bridge_out_done(nbytes);
}

void spi_bridge_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    if (adc_stream_enabled()) adc_stream_complete();
    else spi_bridge_in_done(nbytes);
}

struct usbd_endpoint spi_bridge_out_ep = {
    .ep_addr = SPI_OUT_EP,
    .ep_cb = spi_bridge_out_callback,
};

struct usbd_endpoint spi_bridge_in_ep = {
    .ep_addr = SPI_IN_EP,
    .ep_cb = spi_bridge_in_callback,
};

struct usbd_interface dap_intf;
struct usbd_interface cdc_intf1;
struct usbd_interface cdc_intf2;
#if CONFIG_CHERRYDAP_USE_CUSTOM_HID
struct usbd_interface hid_intf;
#endif
#if CONFIG_CHERRYDAP_USE_MSC
struct usbd_interface msc_intf;
#endif
#if USBD_DFU_RUNTIME_ENABLE
struct usbd_interface dfu_intf;
#endif

struct usb_msosv2_descriptor msosv2_desc = {
    .vendor_code = USBD_WINUSB_VENDOR_CODE,
    .compat_id = USBD_WinUSBDescriptorSetDescriptor,
    .compat_id_len = USBD_WINUSB_DESC_SET_LEN,
};

struct usb_bos_descriptor bos_desc = {
    .string = USBD_BinaryObjectStoreDescriptor,
    .string_len = USBD_BOS_WTOTALLENGTH};

struct usb_webusb_descriptor webusb_url_desc = {
    .vendor_code = USBD_WEBUSB_VENDOR_CODE,
    .string = (const uint8_t *)&USBD_WebUSBURLDescriptor,
    .string_len = URL_DESCRIPTOR_LENGTH};

const struct usb_descriptor cmsisdap_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .other_speed_descriptor_callback = other_speed_config_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
    .bos_descriptor = &bos_desc,
    .msosv2_descriptor = &msosv2_desc,
    .webusb_url_descriptor = &webusb_url_desc};

static void get_device_serial_number(void)
{
#define OTP_UID_ADDR (88)
#define UID_WORD_COUNT (4)
#define UID_BYTE_COUNT (UID_WORD_COUNT * 4)

    uint32_t uid_words[UID_WORD_COUNT];
    uint8_t *uid_bytes = (uint8_t *)uid_words;

    // read UID data
    for (int i = 0; i < UID_WORD_COUNT; i++)
    {
        uid_words[i] = otp_read_from_ip(OTP_UID_ADDR + i);
    }

    // format to Hex string
    char *ptr = serial_number_dynamic;
    for (int i = 0; i < UID_BYTE_COUNT; i++)
    {
        snprintf(ptr, 3, "%02X", uid_bytes[i]);
        ptr += 2;
    }
}

void chry_dap_init(uint8_t busid, uint32_t reg_base)
{
    chry_ringbuffer_init(&g_uartrx, uartrx_ringbuffer, CONFIG_UARTRX_RINGBUF_SIZE);
    chry_ringbuffer_init(&g_usbrx, usbrx_ringbuffer, CONFIG_USBRX_RINGBUF_SIZE);
#if CONFIG_CHERRYDAP_DAP_CMD_ENABLE
    DAP_Setup();
#endif

    get_device_serial_number();

    usbd_desc_register(0, &cmsisdap_descriptor);

    /*!< winusb */
    usbd_add_interface(0, &dap_intf);
    usbd_add_endpoint(0, &dap_out_ep);
    usbd_add_endpoint(0, &dap_in_ep);
    usbd_add_endpoint(0, &swo_in_ep);

    /*!< cdc acm */
    usbd_add_interface(0, usbd_cdc_acm_init_intf(0, &cdc_intf1));
    usbd_add_interface(0, usbd_cdc_acm_init_intf(0, &cdc_intf2));
    usbd_add_endpoint(0, &cdc_out_ep);
    usbd_add_endpoint(0, &cdc_in_ep);

#if CONFIG_CHERRYDAP_USE_CUSTOM_HID
    /*!< hid */
    usbd_add_interface(0, usbd_hid_init_intf(0, &hid_intf, hid_custom_report_desc, HID_CUSTOM_REPORT_DESC_SIZE));
    hid_intf.notify_handler = hid_custom_notify_handler;
    usbd_add_endpoint(0, &hid_custom_in_ep);
    usbd_add_endpoint(0, &hid_custom_out_ep);
#endif

#if SPI_BRIDGE_ENABLE
    /*!< USB→SPI/QSPI 桥：只需登记端点，接口没有类驱动（vendor specific） */
    usbd_add_endpoint(0, &spi_bridge_out_ep);
    usbd_add_endpoint(0, &spi_bridge_in_ep);
#endif

#if CONFIG_CHERRYDAP_USE_MSC
    usbd_add_interface(0, usbd_msc_init_intf(0, &msc_intf, MSC_OUT_EP, MSC_IN_EP));
#endif

#if USBD_DFU_RUNTIME_ENABLE
    /*!< DFU runtime — registered last; intf_num is overridden to match the
     * descriptor position (after the unregistered WebUSB interface). */
    usbd_add_interface(0, &dfu_intf);
    dfu_intf.class_interface_handler = dfu_runtime_handler;
    dfu_intf.intf_num = DFU_INTF_NUM;
#endif

    usbd_initialize(busid, reg_base, usbd_event_handler);
}

#if 1
void chry_dap_handle(void)
{
    uint32_t n;

    // Process pending requests
    while (USB_RequestCountI != USB_RequestCountO) {
        /* 本轮命令的复位代数：执行完再比一次，变了说明这条命令已被总线复位作废
         * （索引/计数/在飞状态都被 ISR 归零了）。见 USB_ResetGen 的说明。 */
        uint16_t gen = USB_ResetGen;

        /* The probe-side RTT bridge shares the SWD bus, so tell it that the
         * DAP is busy again. */
        rtt_bridge_note_dap_activity();

        // Handle Queue Commands
        n = USB_RequestIndexO;
        while (USB_Request[n][0] == ID_DAP_QueueCommands) {
            USB_Request[n][0] = ID_DAP_ExecuteCommands;
            n++;
            if (n == DAP_PACKET_COUNT) {
                n = 0U;
            }
            if (n == USB_RequestIndexI) {
                // flags = osThreadFlagsWait(0x81U, osFlagsWaitAny, osWaitForever);
                // if (flags & 0x80U) {
                //     break;
                // }
            }
        }

        // Execute DAP Command (process request and prepare response)
        USB_RespSize[USB_ResponseIndexI] =
            (uint16_t)DAP_ExecuteCommand(USB_Request[USB_RequestIndexO], USB_Response[USB_ResponseIndexI]);
        /* Bring-up debugging: record the host's DAP bytes (see rtt_bridge.c). */
        rtt_bridge_trace_dap(USB_Request[USB_RequestIndexO], USB_Response[USB_ResponseIndexI]);

        /* ---- 记账：必须在关中断下与复位代数一起原子完成 ----
         * 复位中断可能插在"执行命令"和"记账"之间。它把四个计数清零了，我们要是照样
         * 加一，CountO 就永远比 CountI 大 1 —— 这个差不会再归位，while 会一路空转、
         * 反复执行缓冲里的陈旧命令（可能含对目标的写），直到 16 位回绕才停。
         * 代数变了 → 本轮作废：不加计数，while 条件立刻为假（两个计数都是 0），干净退出。 */
        uint32_t irq_level = disable_global_irq(CSR_MSTATUS_MIE_MASK);
        if (USB_ResetGen != gen) {
            restore_global_irq(irq_level);
            continue;
        }

        // Update Request Index and Count
        USB_RequestIndexO++;
        if (USB_RequestIndexO == DAP_PACKET_COUNT) {
            USB_RequestIndexO = 0U;
        }
        USB_RequestCountO++;

        if (USB_RequestIdle) {
            if ((uint16_t)(USB_RequestCountI - USB_RequestCountO) != DAP_PACKET_COUNT) {
                USB_RequestIdle = 0U;
                usbd_ep_start_read(0, DAP_OUT_EP, USB_Request[USB_RequestIndexI], DAP_XFER_SIZE);
            }
        }

        // Update Response Index and Count
        USB_ResponseIndexI++;
        if (USB_ResponseIndexI == DAP_PACKET_COUNT) {
            USB_ResponseIndexI = 0U;
        }
        USB_ResponseCountI++;

        restore_global_irq(irq_level);

        if (USB_ResponseIdle) {
            if (USB_ResponseCountI != USB_ResponseCountO) {
                // Load data from response buffer to be sent back
                n = USB_ResponseIndexO++;
                if (USB_ResponseIndexO == DAP_PACKET_COUNT) {
                    USB_ResponseIndexO = 0U;
                }
                USB_ResponseCountO++;
                USB_ResponseIdle = 0U;
                usbd_ep_start_write(0, DAP_IN_EP, USB_Response[n], USB_RespSize[n]);
            }
        }
    }
}
#endif
void usbd_cdc_acm_set_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding *line_coding)
{
    (void)busid;
    /* The host sends SET_LINE_CODING every time it opens the port, almost always
     * with the same values. The UART must still be (re)armed in that case:
     * USBD_EVENT_RESET clears config_uart_transfer, and an unchanged line coding
     * used to leave the bridge idle forever - the COM port accepted data, the
     * bytes piled up in g_usbrx and nothing was ever echoed back. */
    if (line_coding->dwDTERate == 0U)
    {
        /* Some host drivers push an all-zero line coding when the port is
         * closed. It is not a usable configuration, so keep the last real one
         * instead of reconfiguring the UART with a 0 baud divisor. */
        return;
    }
    if (memcmp(line_coding, (uint8_t *)&g_cdc_lincoding, sizeof(struct cdc_line_coding)) != 0)
    {
        memcpy((uint8_t *)&g_cdc_lincoding, line_coding, sizeof(struct cdc_line_coding));
        config_uart = 1;
        config_uart_transfer = 0;
    }
    else if (!config_uart_transfer)
    {
        /* Same parameters as last time but the bridge is not running (fresh
         * boot, USB reset, previous error): reconfigure without losing data. */
        config_uart = 1;
    }
}

void usbd_cdc_acm_get_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding *line_coding)
{
    (void)busid;
    memcpy(line_coding, (uint8_t *)&g_cdc_lincoding, sizeof(struct cdc_line_coding));
}
/* 主循环级 CDC/串口桥总开关。
 *
 * 关掉之后 main() 连 chry_dap_usb2uart_handle() 都不调 —— 每轮省下：
 *   · usb2uart_handler() 的一次关中断 + 一次读 DMA 的 DSTADDR 寄存器（外设总线）
 *   · usb→uart 方向的 chry_ringbuffer_get_used() 与 uart→usb 方向的关中断 + get_used()
 *   · usb rx 方向的 chry_ringbuffer_get_free()
 * 量级是**几百个 CPU 周期**，正是高频采样时缺的那一块（见 docs/scope-page.md 的
 * 「端到端 vs 探针能力」一节）。
 *
 * 代价：暂停期间 COM 口不通（CDC 的 bulk OUT 会被 NAK，主机自己重试），
 * 以及 RTT 桥的 USB 转发也会停 —— 但采样器与 RTT 桥本来就互斥，正常用法碰不到。
 * 采样期间数据走的是另一条 bulk IN 0x83，跟这里无关。 */
void chry_dap_usb2uart_set_enabled(uint8_t enable)
{
    if ((enable ? 1U : 0U) == usb2uart_bridge_enabled)
    {
        return;
    }

    if (enable)
    {
        /* 暂停期间 DMA 照转，这里把定位追平并丢掉陈旧数据，再放行主循环。
         * 两个方向的 idle 标志不动：在飞的传输完成回调自己会把它们摆平，
         * 强行置位反而可能在端点上重复起一次读。 */
        uartx_rx_resync();
        usb2uart_bridge_enabled = 1U;
    }
    else
    {
        usb2uart_bridge_enabled = 0U;
    }
}

uint8_t chry_dap_usb2uart_is_enabled(void)
{
    return usb2uart_bridge_enabled;
}

void chry_dap_usb2uart_request_config(void)
{
    /* USB reset/fresh boot must wait for the host's first line coding. */
    if (cdc_configured) { config_uart = 1U; }
}

#if 1
void chry_dap_usb2uart_handle(void)
{
    uint32_t size;
    uint8_t *buffer;

    /* Pull whatever UART2 has received into the ringbuffer first, so the CDC
     * IN path below can forward it without waiting for an idle interrupt. */
    usb2uart_handler();

    if (config_uart)
    {
        /* disable irq here */
        config_uart = 0;
        /* config uart here */
        if (uartx_get_cdc_source() == CDC_SOURCE_UART) {
            chry_dap_usb2uart_uart_config_callback((struct cdc_line_coding *)&g_cdc_lincoding);
        }
        /* A repeated SET_LINE_CODING must not arm CDC IN twice or reset an
         * active stream. UART baud is irrelevant to RTT/SPI producers. */
        if (!cdc_configured) { usbtx_idle_flag = 1; uarttx_idle_flag = 1; cdc_configured = 1U; }
        config_uart_transfer = 1;
        // chry_ringbuffer_reset_read(&g_uartrx);
        /* enable irq here */
    }

    if (config_uart_transfer == 0)
    {
        return;
    }

    /* why we use chry_ringbuffer_linear_read_setup?
     * becase we use dma and we do not want to use temp buffer to memcpy from ringbuffer
     *
     */

    /* uartrx to usb tx */
    if (usbtx_idle_flag)
    {
        uint32_t level = disable_global_irq(CSR_MSTATUS_MIE_MASK);
        if (chry_ringbuffer_get_used(&g_uartrx))
        {
            usbtx_idle_flag = 0;
            /* start first transfer */
            buffer = chry_ringbuffer_linear_read_setup(&g_uartrx, &size);
            usbd_ep_start_write(0, CDC_IN_EP, buffer, size);
        }
        restore_global_irq(level);
    }

    /* usbrx to uart tx */
    if (uarttx_idle_flag)
    {
        if (chry_ringbuffer_get_used(&g_usbrx))
        {
            uarttx_idle_flag = 0;
            /* start first transfer */
            buffer = chry_ringbuffer_linear_read_setup(&g_usbrx, &size);
            chry_dap_usb2uart_uart_send_bydma(buffer, size);
        }
    }

    /* check whether usb rx ringbuffer have space to store */
    if (usbrx_idle_flag)
    {
        if (chry_ringbuffer_get_free(&g_usbrx) >= DAP_PACKET_SIZE)
        {
            usbrx_idle_flag = 0;
            usbd_ep_start_read(0, CDC_OUT_EP, usb_tmpbuffer, DAP_PACKET_SIZE);
        }
    }
}
#endif

/* called by user */
void chry_dap_usb2uart_uart_send_complete(uint32_t size)
{
    uint8_t *buffer;

    chry_ringbuffer_linear_read_done(&g_usbrx, size);

    if (chry_ringbuffer_get_used(&g_usbrx))
    {
        buffer = chry_ringbuffer_linear_read_setup(&g_usbrx, &size);
        chry_dap_usb2uart_uart_send_bydma(buffer, size);
    }
    else
    {
        uarttx_idle_flag = 1;
    }
}
