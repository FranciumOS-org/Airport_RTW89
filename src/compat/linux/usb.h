/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_USB_H
#define _RTW88_COMPAT_USB_H

#include "types.h"
#include "slab.h"
#include "device.h"
#include "workqueue.h"

/* USB vendor/device IDs for rtw88 chips */
#define USB_DEVICE(vend, prod) \
    .idVendor = (vend), .idProduct = (prod), \
    .bcdDevice_lo = 0, .bcdDevice_hi = 0xffff, \
    .bDeviceClass = 0, .bDeviceSubClass = 0, .bDeviceProtocol = 0

struct usb_device_id {
    u16 idVendor;
    u16 idProduct;
    u16 bcdDevice_lo;
    u16 bcdDevice_hi;
    u8  bDeviceClass;
    u8  bDeviceSubClass;
    u8  bDeviceProtocol;
    unsigned long driver_info;
};

/* USB device speed */
enum usb_device_speed {
    USB_SPEED_UNKNOWN = 0,
    USB_SPEED_LOW,
    USB_SPEED_FULL,
    USB_SPEED_HIGH,
    USB_SPEED_WIRELESS,
    USB_SPEED_SUPER,
    USB_SPEED_SUPER_PLUS,
};

/* USB device state */
enum usb_device_state {
    USB_STATE_NOTATTACHED = 0,
    USB_STATE_ATTACHED,
    USB_STATE_POWERED,
    USB_STATE_RECONNECTING,
    USB_STATE_UNAUTHENTICATED,
    USB_STATE_DEFAULT,
    USB_STATE_ADDRESS,
    USB_STATE_CONFIGURED,
    USB_STATE_SUSPENDED,
};

struct usb_device {
    u16 idVendor;
    u16 idProduct;
    enum usb_device_speed speed;
    enum usb_device_state state;
    void *kext_dev;     /* back-pointer to IOUSBDevice */
};

/* USB direction */
#define USB_DIR_IN    0x80
#define USB_DIR_OUT   0x00

/* Transfer types */
#define USB_ENDPOINT_XFER_CONTROL 0
#define USB_ENDPOINT_XFER_ISOC    1
#define USB_ENDPOINT_XFER_BULK    2
#define USB_ENDPOINT_XFER_INT     3

/* USB descriptor types */
struct usb_endpoint_descriptor {
    u8  bLength;
    u8  bDescriptorType;
    u8  bEndpointAddress;
    u8  bmAttributes;
    u16 wMaxPacketSize;
    u8  bInterval;
} __packed;

struct usb_interface_descriptor {
    u8 bLength;
    u8 bDescriptorType;
    u8 bInterfaceNumber;
    u8 bAlternateSetting;
    u8 bNumEndpoints;
    u8 bInterfaceClass;
    u8 bInterfaceSubClass;
    u8 bInterfaceProtocol;
    u8 iInterface;
} __packed;

#define USB_MAX_ENDPOINTS 16

struct usb_host_endpoint {
    struct usb_endpoint_descriptor desc;
};

struct usb_host_interface {
    struct usb_interface_descriptor desc;
    struct usb_host_endpoint endpoint[USB_MAX_ENDPOINTS];
};

struct usb_interface {
    struct usb_device *usb_dev;
    void *driver_data;
    int num_altsetting;
    struct usb_host_interface altsetting[1];
    struct usb_host_interface *cur_altsetting;
    struct device dev;   /* for SET_IEEE80211_DEV(hw, &intf->dev) */
};

/* Endpoint accessor helpers */
static inline int usb_endpoint_num(const struct usb_endpoint_descriptor *epd)
{
    return epd->bEndpointAddress & 0x0f;
}
static inline int usb_endpoint_dir_in(const struct usb_endpoint_descriptor *epd)
{
    return (epd->bEndpointAddress & USB_DIR_IN) != 0;
}
static inline int usb_endpoint_dir_out(const struct usb_endpoint_descriptor *epd)
{
    return !usb_endpoint_dir_in(epd);
}
static inline int usb_endpoint_type(const struct usb_endpoint_descriptor *epd)
{
    return epd->bmAttributes & 0x03;
}
static inline int usb_endpoint_xfer_bulk(const struct usb_endpoint_descriptor *epd)
{
    return usb_endpoint_type(epd) == USB_ENDPOINT_XFER_BULK;
}
static inline int usb_endpoint_xfer_int(const struct usb_endpoint_descriptor *epd)
{
    return usb_endpoint_type(epd) == USB_ENDPOINT_XFER_INT;
}
static inline int usb_endpoint_xfer_isoc(const struct usb_endpoint_descriptor *epd)
{
    return usb_endpoint_type(epd) == USB_ENDPOINT_XFER_ISOC;
}

/* Pipe encoding (opaque token passed back to kext) */
static inline unsigned int usb_sndbulkpipe(struct usb_device *dev, unsigned int ep)
{
    return (USB_ENDPOINT_XFER_BULK << 30) | (ep & 0xf);
}
static inline unsigned int usb_rcvbulkpipe(struct usb_device *dev, unsigned int ep)
{
    return (USB_DIR_IN << 24) | (USB_ENDPOINT_XFER_BULK << 30) | (ep & 0xf);
}
static inline unsigned int usb_sndctrlpipe(struct usb_device *dev, unsigned int ep)
{
    return (USB_ENDPOINT_XFER_CONTROL << 30) | (ep & 0xf);
}
static inline unsigned int usb_rcvctrlpipe(struct usb_device *dev, unsigned int ep)
{
    return (USB_DIR_IN << 24) | (USB_ENDPOINT_XFER_CONTROL << 30) | (ep & 0xf);
}

/* URB */
#define URB_ZERO_PACKET             (1 << 6)
#define URB_NO_TRANSFER_DMA_MAP     (1 << 2)

struct urb;
typedef void (*usb_complete_t)(struct urb *);

struct urb {
    struct usb_device *dev;
    unsigned int pipe;
    void *context;
    usb_complete_t complete;
    void *transfer_buffer;
    u32  transfer_buffer_length;
    u32  actual_length;
    int  status;
    u32  transfer_flags;
    int  start_frame;
    int  error_count;
};

static inline struct urb *usb_alloc_urb(int iso_packets, gfp_t mem_flags)
{
    return (struct urb *)kzalloc(sizeof(struct urb), mem_flags);
}
static inline void usb_free_urb(struct urb *urb) { kfree(urb); }
static inline void usb_kill_urb(struct urb *urb) {}

static inline void usb_fill_bulk_urb(struct urb *urb, struct usb_device *dev,
                                      unsigned int pipe, void *transfer_buffer,
                                      int buffer_length, usb_complete_t complete,
                                      void *context)
{
    urb->dev = dev;
    urb->pipe = pipe;
    urb->transfer_buffer = transfer_buffer;
    urb->transfer_buffer_length = (u32)buffer_length;
    urb->complete = complete;
    urb->context = context;
}

static inline int usb_submit_urb(struct urb *urb, gfp_t mem_flags)
{
    return -EOPNOTSUPP;
}

static inline int usb_reset_device(struct usb_device *dev) { return 0; }

struct usb_ctrlrequest {
    u8  bRequestType;
    u8  bRequest;
    __le16 wValue;
    __le16 wIndex;
    __le16 wLength;
} __packed;

/* USB ops provided by kext */
struct rtw88_usb_ops {
    int (*bulk_msg_out)(struct usb_interface *intf, u8 ep,
                        void *buf, int len, int timeout_ms);
    int (*bulk_msg_in)(struct usb_interface *intf, u8 ep,
                       void *buf, int len, int timeout_ms);
    int (*ctrl_msg)(struct usb_interface *intf,
                    u8 request_type, u8 request,
                    u16 value, u16 index,
                    void *buf, u16 size, int timeout_ms);
    int (*submit_rx_urb)(struct usb_interface *intf, u8 ep,
                         void *buf, int len,
                         void (*complete)(void *ctx, void *buf, int len),
                         void *ctx);
    int (*get_pipe_out)(struct usb_interface *intf, u8 ep);
    int (*get_pipe_in)(struct usb_interface *intf, u8 ep);
};

extern struct rtw88_usb_ops *rtw88_usb_io_ops;

static inline int usb_bulk_msg(struct usb_device *dev, unsigned int pipe,
                                void *data, int len, int *actual_length,
                                int timeout)
{
    return -EOPNOTSUPP;
}

static inline void *usb_get_intfdata(struct usb_interface *intf)
{
    return intf->driver_data;
}

static inline void usb_set_intfdata(struct usb_interface *intf, void *data)
{
    intf->driver_data = data;
}

static inline struct usb_device *interface_to_usbdev(struct usb_interface *intf)
{
    return intf->usb_dev;
}

static inline int usb_set_interface(struct usb_device *dev, int intf, int alt)
{
    return 0;
}

static inline int usb_control_msg(struct usb_device *dev, unsigned int pipe,
                                   __u8 request, __u8 requesttype,
                                   __u16 value, __u16 index,
                                   void *data, __u16 size, int timeout)
{
    return -EOPNOTSUPP;
}

static inline int usb_control_msg_send(struct usb_device *dev, __u8 endpoint,
                                        __u8 request, __u8 requesttype,
                                        __u16 value, __u16 index,
                                        const void *data, __u16 size,
                                        int timeout, gfp_t memflags)
{
    return -EOPNOTSUPP;
}

static inline int usb_control_msg_recv(struct usb_device *dev, __u8 endpoint,
                                        __u8 request, __u8 requesttype,
                                        __u16 value, __u16 index,
                                        void *data, __u16 size,
                                        int timeout, gfp_t memflags)
{
    return -EOPNOTSUPP;
}

struct usb_driver {
    const char *name;
    int (*probe)(struct usb_interface *, const struct usb_device_id *);
    void (*disconnect)(struct usb_interface *);
    int (*suspend)(struct usb_interface *, int message);
    int (*resume)(struct usb_interface *);
    const struct usb_device_id *id_table;
};

static inline int usb_register(struct usb_driver *drv) { return 0; }
static inline void usb_deregister(struct usb_driver *drv) {}

/* IEEE80211 max MPDU length constants */
#define IEEE80211_MAX_MPDU_LEN_VHT_3895   3895
#define IEEE80211_MAX_MPDU_LEN_VHT_7991   7991
#define IEEE80211_MAX_MPDU_LEN_VHT_11454  11454

#define USB_INTERFACE_INFO(cls, sub, prot)
#define USB_DEVICE_AND_INTERFACE_INFO(vend, prod, cl, sc, pr) \
    .idVendor = (vend), .idProduct = (prod), \
    .bcdDevice_lo = 0, .bcdDevice_hi = 0xffff, \
    .bDeviceClass = (cl), .bDeviceSubClass = (sc), .bDeviceProtocol = (pr)

#define module_usb_driver(__usb_driver) \
    static int __init __usb_driver##_init(void) { return usb_register(&(__usb_driver)); } \
    static void __exit __usb_driver##_exit(void) { usb_deregister(&(__usb_driver)); }

#endif /* _RTW88_COMPAT_USB_H */
