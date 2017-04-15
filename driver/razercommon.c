/*
 * Copyright (c) 2015 Tim Theede <pez2001@voyagerproject.de>
 *               2015 Terry Cain <terry@terrys-home.co.uk>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/hid.h>


#include "razercommon.h"

/**
 * Send USB control report to the keyboard
 * USUALLY index = 0x02
 * FIREFLY is 0
 */
int razer_send_control_msg(struct usb_device *usb_dev,void const *data, uint report_index, ulong wait_min, ulong wait_max)
{
    uint request = HID_REQ_SET_REPORT; // 0x09
    uint request_type = USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_OUT; // 0x21
    uint value = 0x300;
    uint size = RAZER_USB_REPORT_LEN;
    char *buf;
    int len;

    buf = kmemdup(data, size, GFP_KERNEL);
    if (buf == NULL)
        return -ENOMEM;

    // Send usb control message
    len = usb_control_msg(usb_dev, usb_sndctrlpipe(usb_dev, 0),
                          request,      // Request
                          request_type, // RequestType
                          value,        // Value
                          report_index, // Index
                          buf,          // Data
                          size,         // Length
                          USB_CTRL_SET_TIMEOUT);

    // Wait
    usleep_range(wait_min, wait_max);

    kfree(buf);
    if(len!=size)
        printk(KERN_WARNING "razer driver: Device data transfer failed.");

    return ((len < 0) ? len : ((len != size) ? -EIO : 0));
}

/**
 * Get a response from the razer device
 *
 * Makes a request like normal, this must change a variable in the device as then we
 * tell it give us data and it gives us a report.
 *
 * Supported Devices:
 *   Razer Chroma
 *   Razer Mamba
 *   Razer BlackWidow Ultimate 2013*
 *   Razer Firefly*
 *
 * Request report is the report sent to the device specifing what response we want
 * Response report will get populated with a response
 *
 * Returns 0 when successful, 1 if the report length is invalid.
 */
int razer_get_usb_response(struct usb_device *usb_dev, uint report_index, struct razer_report* request_report, uint response_index, struct razer_report* response_report, ulong wait_min, ulong wait_max)
{
    uint request = HID_REQ_GET_REPORT; // 0x01
    uint request_type = USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_IN; // 0xA1
    uint value = 0x300;

    uint size = RAZER_USB_REPORT_LEN; // 0x90
    int len;
    int retval;
    int result = 0;
    char *buf;

    buf = kzalloc(sizeof(struct razer_report), GFP_KERNEL);
    if (buf == NULL)
        return -ENOMEM;

    // Send the request to the device.
    // TODO look to see if index needs to be different for the request and the response
    retval = razer_send_control_msg(usb_dev, request_report, report_index, wait_min, wait_max);

    // Now ask for reponse
    len = usb_control_msg(usb_dev, usb_rcvctrlpipe(usb_dev, 0),
                          request,         // Request
                          request_type,    // RequestType
                          value,           // Value
                          response_index,  // Index
                          buf,             // Data
                          size,
                          USB_CTRL_SET_TIMEOUT);

    usleep_range(wait_min, wait_max);

    memcpy(response_report, buf, sizeof(struct razer_report));
    kfree(buf);

    // Error if report is wrong length
    if(len != 90) {
        printk(KERN_WARNING "razer driver: Invalid USB repsonse. USB Report length: %d\n", len);
        result = 1;
    }

    return result;
}

/**
 * Calculate the checksum for the usb message
 *
 * Checksum byte is stored in the 2nd last byte in the messages payload.
 * The checksum is generated by XORing all the bytes in the report starting
 * at byte number 2 (0 based) and ending at byte 88.
 */
unsigned char razer_calculate_crc(struct razer_report *report)
{
    /*second to last byte of report is a simple checksum*/
    /*just xor all bytes up with overflow and you are done*/
    unsigned char crc = 0;
    unsigned char *_report = (unsigned char*)report;

    unsigned int i;
    for(i = 2; i < 88; i++) {
        crc ^= _report[i];
    }

    return crc;
}

/**
 * Get initialised razer report
 */
struct razer_report get_razer_report(unsigned char command_class, unsigned char command_id, unsigned char data_size)
{
    struct razer_report new_report;
    memset(&new_report, 0, sizeof(struct razer_report));

    new_report.status = 0x00;
    new_report.transaction_id.id = 0xFF;
    new_report.remaining_packets = 0x00;
    new_report.protocol_type = 0x00;
    new_report.command_class = command_class;
    new_report.command_id.id = command_id;
    new_report.data_size = data_size;

    return new_report;
}

/**
 * Get empty razer report
 */
struct razer_report get_empty_razer_report(void)
{
    struct razer_report new_report;
    memset(&new_report, 0, sizeof(struct razer_report));

    return new_report;
}

/**
 * Print report to syslog
 */
void print_erroneous_report(struct razer_report* report, char* driver_name, char* message)
{
    printk(KERN_WARNING "%s: %s. Start Marker: %02x id: %02x Num Params: %02x Reserved: %02x Command: %02x Params: %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x .\n",
           driver_name,
           message,
           report->status,
           report->transaction_id.id,
           report->data_size,
           report->command_class,
           report->command_id.id,
           report->arguments[0], report->arguments[1], report->arguments[2], report->arguments[3], report->arguments[4], report->arguments[5],
           report->arguments[6], report->arguments[7], report->arguments[8], report->arguments[9], report->arguments[10], report->arguments[11],
           report->arguments[12], report->arguments[13], report->arguments[14], report->arguments[15]);
}

/**
 * Clamp a value to a min,max
 */
unsigned char clamp_u8(unsigned char value, unsigned char min, unsigned char max)
{
    if(value > max)
        return max;
    if(value < min)
        return min;
    return value;
}
unsigned short clamp_u16(unsigned short value, unsigned short min, unsigned short max)
{
    if(value > max)
        return max;
    if(value < min)
        return min;
    return value;
}


int razer_send_control_msg_old_device(struct usb_device *usb_dev,void const *data, uint report_value, uint report_index, uint report_size, ulong wait_min, ulong wait_max)
{
    uint request = HID_REQ_SET_REPORT; // 0x09
    uint request_type = USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_OUT; // 0x21
    uint value = report_value;
    uint size = report_size;
    char *buf;
    int len;

    buf = kmemdup(data, size, GFP_KERNEL);
    if (buf == NULL)
        return -ENOMEM;

    // Send usb control message
    len = usb_control_msg(usb_dev, usb_sndctrlpipe(usb_dev, 0),
                          request,      // Request
                          request_type, // RequestType
                          value,        // Value
                          report_index, // Index
                          buf,          // Data
                          size,         // Length
                          USB_CTRL_SET_TIMEOUT);

    // Wait
    usleep_range(wait_min, wait_max);

    kfree(buf);
    if(len!=size)
        printk(KERN_WARNING "razer driver: Device data transfer failed.");

    return ((len < 0) ? len : ((len != size) ? -EIO : 0));
}











