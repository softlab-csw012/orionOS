#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    USB_SPEED_FULL  = 0,
    USB_SPEED_LOW   = 1,
    USB_SPEED_HIGH  = 2,
    USB_SPEED_SUPER = 3,
} usb_speed_t;

typedef enum {
    USB_EP_CONTROL   = 0,
    USB_EP_ISOCH     = 1,
    USB_EP_BULK      = 2,
    USB_EP_INTERRUPT = 3,
} usb_ep_type_t;

typedef struct usb_hc usb_hc_t;

typedef struct usb_async_in {
    usb_hc_t* hc;
    void* impl;
} usb_async_in_t;

typedef struct usb_hc_ops {
    bool (*control_transfer)(usb_hc_t* hc, uint32_t dev, uint8_t ep,
                             uint16_t mps, usb_speed_t speed,
                             uint8_t tt_hub_addr, uint8_t tt_port,
                             const void* setup8, void* data, uint16_t len);

    bool (*bulk_transfer)(usb_hc_t* hc, uint32_t dev, uint8_t ep, bool in,
                          uint16_t mps, usb_speed_t speed,
                          uint8_t tt_hub_addr, uint8_t tt_port,
                          uint8_t start_toggle,
                          void* data, uint16_t len);

    bool (*async_in_init)(usb_hc_t* hc, usb_async_in_t* x,
                          uint32_t dev, uint8_t ep, uint16_t mps,
                          usb_speed_t speed,
                          uint8_t tt_hub_addr, uint8_t tt_port,
                          uint8_t start_toggle,
                          void* buf, uint16_t len);

    int (*async_in_check)(usb_async_in_t* x, uint16_t* out_actual);
    void (*async_in_rearm)(usb_async_in_t* x);
    void (*async_in_cancel)(usb_async_in_t* x);

    bool (*configure_endpoint)(usb_hc_t* hc, uint32_t dev,
                               uint8_t ep, bool in,
                               usb_ep_type_t type,
                               uint16_t mps, uint8_t interval);

    bool (*enum_open)(usb_hc_t* hc, uint8_t root_port, usb_speed_t speed,
                      uint32_t* out_dev);

    bool (*enum_set_address)(usb_hc_t* hc, uint32_t dev_default, uint8_t ep0_mps,
                             usb_speed_t speed, uint8_t tt_hub_addr, uint8_t tt_port,
                             uint8_t desired_addr, uint32_t* inout_dev);

    void (*enum_close)(usb_hc_t* hc, uint32_t dev);

    uint8_t (*alloc_address)(usb_hc_t* hc);
    void (*reset_address_allocator)(usb_hc_t* hc);
} usb_hc_ops_t;

struct usb_hc {
    const usb_hc_ops_t* ops;
    void* impl;
};
