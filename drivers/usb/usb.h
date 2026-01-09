#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "usbhc.h"

#define USB_DRIVE_BASE 4
#define USB_MAX_STORAGE_DEVS 4
#ifndef USB_STORAGE_SETTLE_DELAY_MS
#define USB_STORAGE_SETTLE_DELAY_MS 200u
#endif
#ifndef USB_MSC_GET_MAX_LUN_RETRIES
#define USB_MSC_GET_MAX_LUN_RETRIES 2u
#endif
#ifndef USB_MSC_GET_MAX_LUN_RETRY_DELAY_MS
#define USB_MSC_GET_MAX_LUN_RETRY_DELAY_MS 20u
#endif
#ifndef USB_MSC_BOT_RETRIES
#define USB_MSC_BOT_RETRIES 2u
#endif
#ifndef USB_MSC_BOT_RETRY_DELAY_MS
#define USB_MSC_BOT_RETRY_DELAY_MS 20u
#endif
#ifndef USB_MSC_TUR_RETRIES
#define USB_MSC_TUR_RETRIES 5u
#endif
#ifndef USB_MSC_TUR_NOT_READY_DELAY_MS
#define USB_MSC_TUR_NOT_READY_DELAY_MS 50u
#endif
#ifndef USB_MSC_TUR_FAIL_DELAY_MS
#define USB_MSC_TUR_FAIL_DELAY_MS 20u
#endif
#ifndef USB_MSC_READ_CAPACITY_RETRIES
#define USB_MSC_READ_CAPACITY_RETRIES 2u
#endif
#ifndef USB_MSC_READ_CAPACITY_NOT_READY_DELAY_MS
#define USB_MSC_READ_CAPACITY_NOT_READY_DELAY_MS 50u
#endif
#ifndef USB_MSC_READ_CAPACITY_FAIL_DELAY_MS
#define USB_MSC_READ_CAPACITY_FAIL_DELAY_MS 20u
#endif
#ifndef USB_MSC_POST_MAX_LUN_DELAY_MS
#define USB_MSC_POST_MAX_LUN_DELAY_MS 200u
#endif

void usb_port_connected(usb_hc_t* hc, usb_speed_t speed, uint8_t root_port,
                        uint8_t tt_hub_addr, uint8_t tt_port);

void usb_poll(void);
void usb_hid_reset(void);
void usb_hid_drop_device(usb_hc_t* hc, uint32_t dev);
void usb_drop_controller_devices(usb_hc_t* hc);

void usb_storage_reset(void);
uint32_t usb_storage_device_count(void);
void usb_storage_drop_device(usb_hc_t* hc, uint32_t dev);

bool usb_storage_read_sectors(uint8_t drive, uint32_t lba, uint16_t count, uint8_t* buffer);
bool usb_storage_write_sectors(uint8_t drive, uint32_t lba, uint16_t count, const uint8_t* buffer);
uint32_t usb_storage_get_sector_count(uint8_t drive);
uint32_t usb_storage_get_sector_size(uint8_t drive);
bool usb_storage_sync(uint8_t drive);
