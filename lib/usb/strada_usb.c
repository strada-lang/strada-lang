/*
 * strada_usb.c - libusb wrapper for Strada FFI
 *
 * This library provides USB device access from Strada via libusb-1.0.
 * All functions accept StradaValue* arguments directly from Strada's FFI.
 *
 * Compile with:
 *   gcc -shared -fPIC -o libstrada_usb.so strada_usb.c -lusb-1.0 -I../../runtime
 *
 * Requirements:
 *   Ubuntu/Debian: apt install libusb-1.0-0-dev
 *   RHEL/CentOS:   yum install libusb1-devel
 *   macOS:         brew install libusb
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb-1.0/libusb.h>

/* Include Strada runtime for StradaValue type */
#include "strada_runtime.h"

/* Global libusb context */
static libusb_context *usb_ctx = NULL;
static int usb_initialized = 0;

/* ===== Initialization ===== */

/* Initialize libusb
 * Returns 0 on success, negative error code on failure */
int strada_usb_init(void) {
    if (usb_initialized) {
        return 0;
    }
    int ret = libusb_init(&usb_ctx);
    if (ret == 0) {
        usb_initialized = 1;
    }
    return ret;
}

/* Cleanup libusb */
void strada_usb_exit(void) {
    if (usb_initialized && usb_ctx) {
        libusb_exit(usb_ctx);
        usb_ctx = NULL;
        usb_initialized = 0;
    }
}

/* Set debug level (0=none, 1=error, 2=warning, 3=info, 4=debug) */
void strada_usb_set_debug(StradaValue *level_sv) {
    if (!usb_initialized) strada_usb_init();
    int level = (int)strada_to_int(level_sv);
#if LIBUSB_API_VERSION >= 0x01000106
    libusb_set_option(usb_ctx, LIBUSB_OPTION_LOG_LEVEL, level);
#else
    libusb_set_debug(usb_ctx, level);
#endif
}

/* Get libusb error string */
StradaValue* strada_usb_strerror(StradaValue *errcode_sv) {
    int errcode = (int)strada_to_int(errcode_sv);
    const char *str = libusb_strerror(errcode);
    return strada_new_str(str ? str : "Unknown error");
}

/* ===== Device Enumeration ===== */

/* Get list of USB devices
 * Returns array of device info hashes with vid, pid, bus, address */
StradaValue* strada_usb_get_device_list(void) {
    if (!usb_initialized) strada_usb_init();

    libusb_device **list;
    ssize_t count = libusb_get_device_list(usb_ctx, &list);

    StradaValue *result = strada_new_array();

    if (count < 0) {
        return result;
    }

    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) == 0) {
            StradaValue *dev_info = strada_new_hash();

            strada_hash_set(dev_info->value.hv, "vid",
                strada_new_int(desc.idVendor));
            strada_hash_set(dev_info->value.hv, "pid",
                strada_new_int(desc.idProduct));
            strada_hash_set(dev_info->value.hv, "bus",
                strada_new_int(libusb_get_bus_number(list[i])));
            strada_hash_set(dev_info->value.hv, "address",
                strada_new_int(libusb_get_device_address(list[i])));
            strada_hash_set(dev_info->value.hv, "class",
                strada_new_int(desc.bDeviceClass));
            strada_hash_set(dev_info->value.hv, "subclass",
                strada_new_int(desc.bDeviceSubClass));
            strada_hash_set(dev_info->value.hv, "protocol",
                strada_new_int(desc.bDeviceProtocol));

            /* Format VID:PID string */
            char vidpid[16];
            snprintf(vidpid, sizeof(vidpid), "%04x:%04x",
                desc.idVendor, desc.idProduct);
            strada_hash_set(dev_info->value.hv, "vidpid",
                strada_new_str(vidpid));

            strada_array_push(result->value.av, dev_info);
        }
    }

    libusb_free_device_list(list, 1);
    return result;
}

/* ===== Device Open/Close ===== */

/* Open device by VID:PID
 * Returns device handle (as int) or 0 on failure */
StradaValue* strada_usb_open_device(StradaValue *vid_sv, StradaValue *pid_sv) {
    if (!usb_initialized) strada_usb_init();

    uint16_t vid = (uint16_t)strada_to_int(vid_sv);
    uint16_t pid = (uint16_t)strada_to_int(pid_sv);

    libusb_device_handle *handle = libusb_open_device_with_vid_pid(usb_ctx, vid, pid);
    if (!handle) {
        return strada_new_int(0);
    }

    return strada_new_int((int64_t)(intptr_t)handle);
}

/* Open device by bus and address
 * Returns device handle or 0 on failure */
StradaValue* strada_usb_open_device_by_path(StradaValue *bus_sv, StradaValue *addr_sv) {
    if (!usb_initialized) strada_usb_init();

    int target_bus = (int)strada_to_int(bus_sv);
    int target_addr = (int)strada_to_int(addr_sv);

    libusb_device **list;
    ssize_t count = libusb_get_device_list(usb_ctx, &list);
    if (count < 0) {
        return strada_new_int(0);
    }

    libusb_device_handle *handle = NULL;

    for (ssize_t i = 0; i < count; i++) {
        int bus = libusb_get_bus_number(list[i]);
        int addr = libusb_get_device_address(list[i]);

        if (bus == target_bus && addr == target_addr) {
            if (libusb_open(list[i], &handle) != 0) {
                handle = NULL;
            }
            break;
        }
    }

    libusb_free_device_list(list, 1);
    return strada_new_int((int64_t)(intptr_t)handle);
}

/* Close device handle */
void strada_usb_close(StradaValue *handle_sv) {
    libusb_device_handle *handle = (libusb_device_handle *)(intptr_t)strada_to_int(handle_sv);
    if (handle) {
        libusb_close(handle);
    }
}

/* ===== Interface Management ===== */

/* Claim interface for exclusive use
 * Returns 0 on success, negative error code on failure */
StradaValue* strada_usb_claim_interface(StradaValue *handle_sv, StradaValue *iface_sv) {
    libusb_device_handle *handle = (libusb_device_handle *)(intptr_t)strada_to_int(handle_sv);
    int iface = (int)strada_to_int(iface_sv);

    if (!handle) {
        return strada_new_int(LIBUSB_ERROR_INVALID_PARAM);
    }

    return strada_new_int(libusb_claim_interface(handle, iface));
}

/* Release interface */
StradaValue* strada_usb_release_interface(StradaValue *handle_sv, StradaValue *iface_sv) {
    libusb_device_handle *handle = (libusb_device_handle *)(intptr_t)strada_to_int(handle_sv);
    int iface = (int)strada_to_int(iface_sv);

    if (!handle) {
        return strada_new_int(LIBUSB_ERROR_INVALID_PARAM);
    }

    return strada_new_int(libusb_release_interface(handle, iface));
}

/* Detach kernel driver from interface (Linux only) */
StradaValue* strada_usb_detach_kernel_driver(StradaValue *handle_sv, StradaValue *iface_sv) {
    libusb_device_handle *handle = (libusb_device_handle *)(intptr_t)strada_to_int(handle_sv);
    int iface = (int)strada_to_int(iface_sv);

    if (!handle) {
        return strada_new_int(LIBUSB_ERROR_INVALID_PARAM);
    }

    return strada_new_int(libusb_detach_kernel_driver(handle, iface));
}

/* Check if kernel driver is active on interface */
StradaValue* strada_usb_kernel_driver_active(StradaValue *handle_sv, StradaValue *iface_sv) {
    libusb_device_handle *handle = (libusb_device_handle *)(intptr_t)strada_to_int(handle_sv);
    int iface = (int)strada_to_int(iface_sv);

    if (!handle) {
        return strada_new_int(LIBUSB_ERROR_INVALID_PARAM);
    }

    return strada_new_int(libusb_kernel_driver_active(handle, iface));
}

/* Set auto-detach kernel driver mode */
StradaValue* strada_usb_set_auto_detach(StradaValue *handle_sv, StradaValue *enable_sv) {
    libusb_device_handle *handle = (libusb_device_handle *)(intptr_t)strada_to_int(handle_sv);
    int enable = (int)strada_to_int(enable_sv);

    if (!handle) {
        return strada_new_int(LIBUSB_ERROR_INVALID_PARAM);
    }

    return strada_new_int(libusb_set_auto_detach_kernel_driver(handle, enable));
}

/* ===== Data Transfers ===== */

/* Bulk transfer (read or write based on endpoint direction)
 * endpoint: 0x01-0x0F for OUT, 0x81-0x8F for IN
 * Returns: data string on read, bytes transferred count on write */
StradaValue* strada_usb_bulk_transfer(StradaValue *handle_sv, StradaValue *endpoint_sv,
                                       StradaValue *data_sv, StradaValue *timeout_sv) {
    libusb_device_handle *handle = (libusb_device_handle *)(intptr_t)strada_to_int(handle_sv);
    unsigned char endpoint = (unsigned char)strada_to_int(endpoint_sv);
    int timeout = (int)strada_to_int(timeout_sv);

    if (!handle) {
        return strada_new_int(LIBUSB_ERROR_INVALID_PARAM);
    }

    int transferred = 0;

    /* Check endpoint direction */
    if (endpoint & 0x80) {
        /* IN endpoint - read data */
        int length = (int)strada_to_int(data_sv);  /* data_sv is the length for reads */
        unsigned char *buffer = malloc(length);
        if (!buffer) {
            return strada_new_str("");
        }

        int ret = libusb_bulk_transfer(handle, endpoint, buffer, length, &transferred, timeout);
        if (ret < 0 && ret != LIBUSB_ERROR_TIMEOUT) {
            free(buffer);
            return strada_new_str("");
        }

        StradaValue *result = strada_new_str_len((char *)buffer, transferred);
        free(buffer);
        return result;
    } else {
        /* OUT endpoint - write data */
        const char *data = strada_to_str(data_sv);
        int length = data_sv->struct_size > 0 ? (int)data_sv->struct_size : (int)strlen(data);

        int ret = libusb_bulk_transfer(handle, endpoint, (unsigned char *)data,
                                        length, &transferred, timeout);
        if (ret < 0) {
            return strada_new_int(ret);
        }
        return strada_new_int(transferred);
    }
}

/* Interrupt transfer */
StradaValue* strada_usb_interrupt_transfer(StradaValue *handle_sv, StradaValue *endpoint_sv,
                                            StradaValue *data_sv, StradaValue *timeout_sv) {
    libusb_device_handle *handle = (libusb_device_handle *)(intptr_t)strada_to_int(handle_sv);
    unsigned char endpoint = (unsigned char)strada_to_int(endpoint_sv);
    int timeout = (int)strada_to_int(timeout_sv);

    if (!handle) {
        return strada_new_int(LIBUSB_ERROR_INVALID_PARAM);
    }

    int transferred = 0;

    if (endpoint & 0x80) {
        /* IN endpoint - read */
        int length = (int)strada_to_int(data_sv);
        unsigned char *buffer = malloc(length);
        if (!buffer) {
            return strada_new_str("");
        }

        int ret = libusb_interrupt_transfer(handle, endpoint, buffer, length, &transferred, timeout);
        if (ret < 0 && ret != LIBUSB_ERROR_TIMEOUT) {
            free(buffer);
            return strada_new_str("");
        }

        StradaValue *result = strada_new_str_len((char *)buffer, transferred);
        free(buffer);
        return result;
    } else {
        /* OUT endpoint - write */
        const char *data = strada_to_str(data_sv);
        int length = data_sv->struct_size > 0 ? (int)data_sv->struct_size : (int)strlen(data);

        int ret = libusb_interrupt_transfer(handle, endpoint, (unsigned char *)data,
                                             length, &transferred, timeout);
        if (ret < 0) {
            return strada_new_int(ret);
        }
        return strada_new_int(transferred);
    }
}

/* Control transfer
 * For vendor/class specific requests */
StradaValue* strada_usb_control_transfer(StradaValue *handle_sv,
                                          StradaValue *request_type_sv,
                                          StradaValue *request_sv,
                                          StradaValue *value_sv,
                                          StradaValue *index_sv,
                                          StradaValue *data_sv,
                                          StradaValue *timeout_sv) {
    libusb_device_handle *handle = (libusb_device_handle *)(intptr_t)strada_to_int(handle_sv);
    uint8_t request_type = (uint8_t)strada_to_int(request_type_sv);
    uint8_t request = (uint8_t)strada_to_int(request_sv);
    uint16_t value = (uint16_t)strada_to_int(value_sv);
    uint16_t index = (uint16_t)strada_to_int(index_sv);
    unsigned int timeout = (unsigned int)strada_to_int(timeout_sv);

    if (!handle) {
        return strada_new_int(LIBUSB_ERROR_INVALID_PARAM);
    }

    /* Check if this is a read or write based on request_type bit 7 */
    if (request_type & 0x80) {
        /* Device-to-host (read) */
        int length = (int)strada_to_int(data_sv);
        unsigned char *buffer = malloc(length);
        if (!buffer) {
            return strada_new_str("");
        }

        int ret = libusb_control_transfer(handle, request_type, request,
                                           value, index, buffer, length, timeout);
        if (ret < 0) {
            free(buffer);
            return strada_new_str("");
        }

        StradaValue *result = strada_new_str_len((char *)buffer, ret);
        free(buffer);
        return result;
    } else {
        /* Host-to-device (write) */
        const char *data = strada_to_str(data_sv);
        int length = data_sv->struct_size > 0 ? (int)data_sv->struct_size : (int)strlen(data);

        int ret = libusb_control_transfer(handle, request_type, request,
                                           value, index, (unsigned char *)data, length, timeout);
        return strada_new_int(ret);
    }
}

/* ===== Device Descriptors ===== */

/* Get device descriptor
 * Returns hash with device info */
StradaValue* strada_usb_get_device_descriptor(StradaValue *handle_sv) {
    libusb_device_handle *handle = (libusb_device_handle *)(intptr_t)strada_to_int(handle_sv);
    if (!handle) {
        return strada_new_undef();
    }

    libusb_device *dev = libusb_get_device(handle);
    struct libusb_device_descriptor desc;

    if (libusb_get_device_descriptor(dev, &desc) != 0) {
        return strada_new_undef();
    }

    StradaValue *result = strada_new_hash();
    strada_hash_set(result->value.hv, "usb_version",
        strada_new_int(desc.bcdUSB));
    strada_hash_set(result->value.hv, "device_class",
        strada_new_int(desc.bDeviceClass));
    strada_hash_set(result->value.hv, "device_subclass",
        strada_new_int(desc.bDeviceSubClass));
    strada_hash_set(result->value.hv, "device_protocol",
        strada_new_int(desc.bDeviceProtocol));
    strada_hash_set(result->value.hv, "max_packet_size",
        strada_new_int(desc.bMaxPacketSize0));
    strada_hash_set(result->value.hv, "vendor_id",
        strada_new_int(desc.idVendor));
    strada_hash_set(result->value.hv, "product_id",
        strada_new_int(desc.idProduct));
    strada_hash_set(result->value.hv, "device_version",
        strada_new_int(desc.bcdDevice));
    strada_hash_set(result->value.hv, "num_configurations",
        strada_new_int(desc.bNumConfigurations));
    strada_hash_set(result->value.hv, "manufacturer_index",
        strada_new_int(desc.iManufacturer));
    strada_hash_set(result->value.hv, "product_index",
        strada_new_int(desc.iProduct));
    strada_hash_set(result->value.hv, "serial_index",
        strada_new_int(desc.iSerialNumber));

    return result;
}

/* Get string descriptor */
StradaValue* strada_usb_get_string_descriptor(StradaValue *handle_sv, StradaValue *index_sv) {
    libusb_device_handle *handle = (libusb_device_handle *)(intptr_t)strada_to_int(handle_sv);
    int index = (int)strada_to_int(index_sv);

    if (!handle || index == 0) {
        return strada_new_str("");
    }

    unsigned char buffer[256];
    int ret = libusb_get_string_descriptor_ascii(handle, index, buffer, sizeof(buffer));

    if (ret < 0) {
        return strada_new_str("");
    }

    return strada_new_str((char *)buffer);
}

/* Get configuration descriptor
 * Returns hash with configuration info */
StradaValue* strada_usb_get_config_descriptor(StradaValue *handle_sv, StradaValue *config_sv) {
    libusb_device_handle *handle = (libusb_device_handle *)(intptr_t)strada_to_int(handle_sv);
    int config_index = (int)strada_to_int(config_sv);

    if (!handle) {
        return strada_new_undef();
    }

    libusb_device *dev = libusb_get_device(handle);
    struct libusb_config_descriptor *config;

    if (libusb_get_config_descriptor(dev, config_index, &config) != 0) {
        return strada_new_undef();
    }

    StradaValue *result = strada_new_hash();
    strada_hash_set(result->value.hv, "num_interfaces",
        strada_new_int(config->bNumInterfaces));
    strada_hash_set(result->value.hv, "configuration_value",
        strada_new_int(config->bConfigurationValue));
    strada_hash_set(result->value.hv, "max_power",
        strada_new_int(config->MaxPower * 2));  /* In mA */
    strada_hash_set(result->value.hv, "attributes",
        strada_new_int(config->bmAttributes));

    /* Build array of interfaces */
    StradaValue *interfaces = strada_new_array();
    for (int i = 0; i < config->bNumInterfaces; i++) {
        const struct libusb_interface *iface = &config->interface[i];
        for (int j = 0; j < iface->num_altsetting; j++) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[j];

            StradaValue *iface_info = strada_new_hash();
            strada_hash_set(iface_info->value.hv, "interface_number",
                strada_new_int(alt->bInterfaceNumber));
            strada_hash_set(iface_info->value.hv, "alt_setting",
                strada_new_int(alt->bAlternateSetting));
            strada_hash_set(iface_info->value.hv, "interface_class",
                strada_new_int(alt->bInterfaceClass));
            strada_hash_set(iface_info->value.hv, "interface_subclass",
                strada_new_int(alt->bInterfaceSubClass));
            strada_hash_set(iface_info->value.hv, "interface_protocol",
                strada_new_int(alt->bInterfaceProtocol));
            strada_hash_set(iface_info->value.hv, "num_endpoints",
                strada_new_int(alt->bNumEndpoints));

            /* Build array of endpoints */
            StradaValue *endpoints = strada_new_array();
            for (int k = 0; k < alt->bNumEndpoints; k++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[k];

                StradaValue *ep_info = strada_new_hash();
                strada_hash_set(ep_info->value.hv, "address",
                    strada_new_int(ep->bEndpointAddress));
                strada_hash_set(ep_info->value.hv, "attributes",
                    strada_new_int(ep->bmAttributes));
                strada_hash_set(ep_info->value.hv, "max_packet_size",
                    strada_new_int(ep->wMaxPacketSize));
                strada_hash_set(ep_info->value.hv, "interval",
                    strada_new_int(ep->bInterval));

                /* Decode transfer type */
                const char *type_str = "unknown";
                switch (ep->bmAttributes & 0x03) {
                    case LIBUSB_TRANSFER_TYPE_CONTROL: type_str = "control"; break;
                    case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS: type_str = "isochronous"; break;
                    case LIBUSB_TRANSFER_TYPE_BULK: type_str = "bulk"; break;
                    case LIBUSB_TRANSFER_TYPE_INTERRUPT: type_str = "interrupt"; break;
                }
                strada_hash_set(ep_info->value.hv, "type", strada_new_str(type_str));

                /* Decode direction */
                strada_hash_set(ep_info->value.hv, "direction",
                    strada_new_str((ep->bEndpointAddress & 0x80) ? "in" : "out"));

                strada_array_push(endpoints->value.av, ep_info);
            }
            strada_hash_set(iface_info->value.hv, "endpoints", endpoints);

            strada_array_push(interfaces->value.av, iface_info);
        }
    }
    strada_hash_set(result->value.hv, "interfaces", interfaces);

    libusb_free_config_descriptor(config);
    return result;
}

/* ===== Configuration ===== */

/* Get active configuration */
StradaValue* strada_usb_get_configuration(StradaValue *handle_sv) {
    libusb_device_handle *handle = (libusb_device_handle *)(intptr_t)strada_to_int(handle_sv);
    if (!handle) {
        return strada_new_int(LIBUSB_ERROR_INVALID_PARAM);
    }

    int config;
    int ret = libusb_get_configuration(handle, &config);
    if (ret < 0) {
        return strada_new_int(ret);
    }
    return strada_new_int(config);
}

/* Set configuration */
StradaValue* strada_usb_set_configuration(StradaValue *handle_sv, StradaValue *config_sv) {
    libusb_device_handle *handle = (libusb_device_handle *)(intptr_t)strada_to_int(handle_sv);
    int config = (int)strada_to_int(config_sv);

    if (!handle) {
        return strada_new_int(LIBUSB_ERROR_INVALID_PARAM);
    }

    return strada_new_int(libusb_set_configuration(handle, config));
}

/* Set alternate setting for interface */
StradaValue* strada_usb_set_interface_alt_setting(StradaValue *handle_sv,
                                                    StradaValue *iface_sv,
                                                    StradaValue *alt_sv) {
    libusb_device_handle *handle = (libusb_device_handle *)(intptr_t)strada_to_int(handle_sv);
    int iface = (int)strada_to_int(iface_sv);
    int alt = (int)strada_to_int(alt_sv);

    if (!handle) {
        return strada_new_int(LIBUSB_ERROR_INVALID_PARAM);
    }

    return strada_new_int(libusb_set_interface_alt_setting(handle, iface, alt));
}

/* Clear halt/stall on endpoint */
StradaValue* strada_usb_clear_halt(StradaValue *handle_sv, StradaValue *endpoint_sv) {
    libusb_device_handle *handle = (libusb_device_handle *)(intptr_t)strada_to_int(handle_sv);
    unsigned char endpoint = (unsigned char)strada_to_int(endpoint_sv);

    if (!handle) {
        return strada_new_int(LIBUSB_ERROR_INVALID_PARAM);
    }

    return strada_new_int(libusb_clear_halt(handle, endpoint));
}

/* Reset device */
StradaValue* strada_usb_reset_device(StradaValue *handle_sv) {
    libusb_device_handle *handle = (libusb_device_handle *)(intptr_t)strada_to_int(handle_sv);
    if (!handle) {
        return strada_new_int(LIBUSB_ERROR_INVALID_PARAM);
    }

    return strada_new_int(libusb_reset_device(handle));
}

/* ===== USB Class Constants ===== */

/* Return USB class code constants */
StradaValue* strada_usb_class_hid(void) { return strada_new_int(LIBUSB_CLASS_HID); }
StradaValue* strada_usb_class_mass_storage(void) { return strada_new_int(LIBUSB_CLASS_MASS_STORAGE); }
StradaValue* strada_usb_class_hub(void) { return strada_new_int(LIBUSB_CLASS_HUB); }
StradaValue* strada_usb_class_vendor_spec(void) { return strada_new_int(LIBUSB_CLASS_VENDOR_SPEC); }
StradaValue* strada_usb_class_printer(void) { return strada_new_int(LIBUSB_CLASS_PRINTER); }
StradaValue* strada_usb_class_audio(void) { return strada_new_int(LIBUSB_CLASS_AUDIO); }
StradaValue* strada_usb_class_video(void) { return strada_new_int(LIBUSB_CLASS_VIDEO); }
StradaValue* strada_usb_class_comm(void) { return strada_new_int(LIBUSB_CLASS_COMM); }

/* Request type constants for control transfers */
StradaValue* strada_usb_request_type_standard(void) { return strada_new_int(LIBUSB_REQUEST_TYPE_STANDARD); }
StradaValue* strada_usb_request_type_class(void) { return strada_new_int(LIBUSB_REQUEST_TYPE_CLASS); }
StradaValue* strada_usb_request_type_vendor(void) { return strada_new_int(LIBUSB_REQUEST_TYPE_VENDOR); }
StradaValue* strada_usb_recipient_device(void) { return strada_new_int(LIBUSB_RECIPIENT_DEVICE); }
StradaValue* strada_usb_recipient_interface(void) { return strada_new_int(LIBUSB_RECIPIENT_INTERFACE); }
StradaValue* strada_usb_recipient_endpoint(void) { return strada_new_int(LIBUSB_RECIPIENT_ENDPOINT); }
StradaValue* strada_usb_endpoint_in(void) { return strada_new_int(LIBUSB_ENDPOINT_IN); }
StradaValue* strada_usb_endpoint_out(void) { return strada_new_int(LIBUSB_ENDPOINT_OUT); }

/* Standard request codes */
StradaValue* strada_usb_request_get_status(void) { return strada_new_int(LIBUSB_REQUEST_GET_STATUS); }
StradaValue* strada_usb_request_clear_feature(void) { return strada_new_int(LIBUSB_REQUEST_CLEAR_FEATURE); }
StradaValue* strada_usb_request_set_feature(void) { return strada_new_int(LIBUSB_REQUEST_SET_FEATURE); }
StradaValue* strada_usb_request_get_descriptor(void) { return strada_new_int(LIBUSB_REQUEST_GET_DESCRIPTOR); }
StradaValue* strada_usb_request_set_descriptor(void) { return strada_new_int(LIBUSB_REQUEST_SET_DESCRIPTOR); }
StradaValue* strada_usb_request_get_configuration(void) { return strada_new_int(LIBUSB_REQUEST_GET_CONFIGURATION); }
StradaValue* strada_usb_request_set_configuration(void) { return strada_new_int(LIBUSB_REQUEST_SET_CONFIGURATION); }

/* Error code constants */
StradaValue* strada_usb_error_io(void) { return strada_new_int(LIBUSB_ERROR_IO); }
StradaValue* strada_usb_error_invalid_param(void) { return strada_new_int(LIBUSB_ERROR_INVALID_PARAM); }
StradaValue* strada_usb_error_access(void) { return strada_new_int(LIBUSB_ERROR_ACCESS); }
StradaValue* strada_usb_error_no_device(void) { return strada_new_int(LIBUSB_ERROR_NO_DEVICE); }
StradaValue* strada_usb_error_not_found(void) { return strada_new_int(LIBUSB_ERROR_NOT_FOUND); }
StradaValue* strada_usb_error_busy(void) { return strada_new_int(LIBUSB_ERROR_BUSY); }
StradaValue* strada_usb_error_timeout(void) { return strada_new_int(LIBUSB_ERROR_TIMEOUT); }
StradaValue* strada_usb_error_overflow(void) { return strada_new_int(LIBUSB_ERROR_OVERFLOW); }
StradaValue* strada_usb_error_pipe(void) { return strada_new_int(LIBUSB_ERROR_PIPE); }
StradaValue* strada_usb_error_interrupted(void) { return strada_new_int(LIBUSB_ERROR_INTERRUPTED); }
StradaValue* strada_usb_error_no_mem(void) { return strada_new_int(LIBUSB_ERROR_NO_MEM); }
StradaValue* strada_usb_error_not_supported(void) { return strada_new_int(LIBUSB_ERROR_NOT_SUPPORTED); }

/* ============== Raw C Functions for extern "C" ============== */

/* Cached device list for iteration */
static libusb_device **cached_device_list = NULL;
static ssize_t cached_device_count = 0;

/* Refresh the device list cache */
int strada_usb_refresh_device_list_raw(void) {
    if (!usb_initialized) strada_usb_init();

    /* Free old list if exists */
    if (cached_device_list) {
        libusb_free_device_list(cached_device_list, 1);
        cached_device_list = NULL;
        cached_device_count = 0;
    }

    cached_device_count = libusb_get_device_list(usb_ctx, &cached_device_list);
    if (cached_device_count < 0) {
        cached_device_count = 0;
        return -1;
    }
    return (int)cached_device_count;
}

/* Get device count */
int strada_usb_device_count_raw(void) {
    return (int)cached_device_count;
}

/* Get device VID at index */
int strada_usb_device_vid_raw(int idx) {
    if (idx < 0 || idx >= cached_device_count) return 0;
    struct libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(cached_device_list[idx], &desc) != 0) return 0;
    return desc.idVendor;
}

/* Get device PID at index */
int strada_usb_device_pid_raw(int idx) {
    if (idx < 0 || idx >= cached_device_count) return 0;
    struct libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(cached_device_list[idx], &desc) != 0) return 0;
    return desc.idProduct;
}

/* Get device bus number at index */
int strada_usb_device_bus_raw(int idx) {
    if (idx < 0 || idx >= cached_device_count) return 0;
    return libusb_get_bus_number(cached_device_list[idx]);
}

/* Get device address at index */
int strada_usb_device_addr_raw(int idx) {
    if (idx < 0 || idx >= cached_device_count) return 0;
    return libusb_get_device_address(cached_device_list[idx]);
}

/* Get device class at index */
int strada_usb_device_class_raw(int idx) {
    if (idx < 0 || idx >= cached_device_count) return 0;
    struct libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(cached_device_list[idx], &desc) != 0) return 0;
    return desc.bDeviceClass;
}

/* Get device VID:PID string at index */
char* strada_usb_device_vidpid_raw(int idx) {
    if (idx < 0 || idx >= cached_device_count) return strdup("");
    struct libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(cached_device_list[idx], &desc) != 0) return strdup("");
    char buf[16];
    snprintf(buf, sizeof(buf), "%04x:%04x", desc.idVendor, desc.idProduct);
    return strdup(buf);
}

/* Free cached device list */
void strada_usb_free_device_list_raw(void) {
    if (cached_device_list) {
        libusb_free_device_list(cached_device_list, 1);
        cached_device_list = NULL;
        cached_device_count = 0;
    }
}

/* Open device by VID:PID - returns handle as pointer */
void* strada_usb_open_device_raw(int vid, int pid) {
    if (!usb_initialized) strada_usb_init();
    return libusb_open_device_with_vid_pid(usb_ctx, (uint16_t)vid, (uint16_t)pid);
}

/* Open device by bus/address - returns handle as pointer */
void* strada_usb_open_device_by_path_raw(int bus, int addr) {
    if (!usb_initialized) strada_usb_init();

    libusb_device **list;
    ssize_t count = libusb_get_device_list(usb_ctx, &list);
    if (count < 0) return NULL;

    libusb_device_handle *handle = NULL;
    for (ssize_t i = 0; i < count; i++) {
        if (libusb_get_bus_number(list[i]) == bus &&
            libusb_get_device_address(list[i]) == addr) {
            if (libusb_open(list[i], &handle) != 0) {
                handle = NULL;
            }
            break;
        }
    }

    libusb_free_device_list(list, 1);
    return handle;
}

/* Close device handle */
void strada_usb_close_raw(void *handle) {
    if (handle) {
        libusb_close((libusb_device_handle *)handle);
    }
}

/* Claim interface */
int strada_usb_claim_interface_raw(void *handle, int iface) {
    if (!handle) return LIBUSB_ERROR_INVALID_PARAM;
    return libusb_claim_interface((libusb_device_handle *)handle, iface);
}

/* Release interface */
int strada_usb_release_interface_raw(void *handle, int iface) {
    if (!handle) return LIBUSB_ERROR_INVALID_PARAM;
    return libusb_release_interface((libusb_device_handle *)handle, iface);
}

/* Detach kernel driver */
int strada_usb_detach_kernel_driver_raw(void *handle, int iface) {
    if (!handle) return LIBUSB_ERROR_INVALID_PARAM;
    return libusb_detach_kernel_driver((libusb_device_handle *)handle, iface);
}

/* Check kernel driver active */
int strada_usb_kernel_driver_active_raw(void *handle, int iface) {
    if (!handle) return LIBUSB_ERROR_INVALID_PARAM;
    return libusb_kernel_driver_active((libusb_device_handle *)handle, iface);
}

/* Set auto detach */
int strada_usb_set_auto_detach_raw(void *handle, int enable) {
    if (!handle) return LIBUSB_ERROR_INVALID_PARAM;
    return libusb_set_auto_detach_kernel_driver((libusb_device_handle *)handle, enable);
}

/* Bulk write - returns bytes transferred or error */
int strada_usb_bulk_write_raw(void *handle, int endpoint, const char *data, int length, int timeout) {
    if (!handle) return LIBUSB_ERROR_INVALID_PARAM;
    int transferred = 0;
    int ret = libusb_bulk_transfer((libusb_device_handle *)handle,
                                    (unsigned char)endpoint,
                                    (unsigned char *)data, length, &transferred, timeout);
    if (ret < 0) return ret;
    return transferred;
}

/* Last transfer length for read operations */
static int last_transfer_len = 0;

/* Get last transfer length */
int strada_usb_last_transfer_len_raw(void) {
    return last_transfer_len;
}

/* Bulk read - returns data, length stored in last_transfer_len */
char* strada_usb_bulk_read_raw(void *handle, int endpoint, int max_len, int timeout) {
    last_transfer_len = 0;
    if (!handle) {
        return NULL;
    }
    unsigned char *buffer = malloc(max_len);
    if (!buffer) {
        return NULL;
    }

    int transferred = 0;
    int ret = libusb_bulk_transfer((libusb_device_handle *)handle,
                                    (unsigned char)(endpoint | 0x80),
                                    buffer, max_len, &transferred, timeout);
    if (ret < 0 && ret != LIBUSB_ERROR_TIMEOUT) {
        free(buffer);
        return NULL;
    }

    last_transfer_len = transferred;
    return (char *)buffer;
}

/* Interrupt write */
int strada_usb_interrupt_write_raw(void *handle, int endpoint, const char *data, int length, int timeout) {
    if (!handle) return LIBUSB_ERROR_INVALID_PARAM;
    int transferred = 0;
    int ret = libusb_interrupt_transfer((libusb_device_handle *)handle,
                                         (unsigned char)endpoint,
                                         (unsigned char *)data, length, &transferred, timeout);
    if (ret < 0) return ret;
    return transferred;
}

/* Interrupt read - length stored in last_transfer_len */
char* strada_usb_interrupt_read_raw(void *handle, int endpoint, int max_len, int timeout) {
    last_transfer_len = 0;
    if (!handle) {
        return NULL;
    }
    unsigned char *buffer = malloc(max_len);
    if (!buffer) {
        return NULL;
    }

    int transferred = 0;
    int ret = libusb_interrupt_transfer((libusb_device_handle *)handle,
                                         (unsigned char)(endpoint | 0x80),
                                         buffer, max_len, &transferred, timeout);
    if (ret < 0 && ret != LIBUSB_ERROR_TIMEOUT) {
        free(buffer);
        return NULL;
    }

    last_transfer_len = transferred;
    return (char *)buffer;
}

/* Control transfer write */
int strada_usb_control_write_raw(void *handle, int request_type, int request,
                                  int value, int idx, const char *data, int length, int timeout) {
    if (!handle) return LIBUSB_ERROR_INVALID_PARAM;
    return libusb_control_transfer((libusb_device_handle *)handle,
                                    (uint8_t)request_type, (uint8_t)request,
                                    (uint16_t)value, (uint16_t)idx,
                                    (unsigned char *)data, (uint16_t)length, timeout);
}

/* Control transfer read - length stored in last_transfer_len */
char* strada_usb_control_read_raw(void *handle, int request_type, int request,
                                   int value, int idx, int max_len, int timeout) {
    last_transfer_len = 0;
    if (!handle) {
        return NULL;
    }
    unsigned char *buffer = malloc(max_len);
    if (!buffer) {
        return NULL;
    }

    int ret = libusb_control_transfer((libusb_device_handle *)handle,
                                       (uint8_t)(request_type | 0x80), (uint8_t)request,
                                       (uint16_t)value, (uint16_t)idx,
                                       buffer, (uint16_t)max_len, timeout);
    if (ret < 0) {
        free(buffer);
        return NULL;
    }

    last_transfer_len = ret;
    return (char *)buffer;
}

/* Device descriptor accessors */
static struct libusb_device_descriptor desc_cache;
static int desc_cache_valid = 0;
static void *desc_cache_handle = NULL;

int strada_usb_get_desc_raw(void *handle) {
    if (!handle) return -1;
    libusb_device *dev = libusb_get_device((libusb_device_handle *)handle);
    int ret = libusb_get_device_descriptor(dev, &desc_cache);
    if (ret == 0) {
        desc_cache_valid = 1;
        desc_cache_handle = handle;
    }
    return ret;
}

int strada_usb_desc_usb_version_raw(void) { return desc_cache_valid ? desc_cache.bcdUSB : 0; }
int strada_usb_desc_device_class_raw(void) { return desc_cache_valid ? desc_cache.bDeviceClass : 0; }
int strada_usb_desc_device_subclass_raw(void) { return desc_cache_valid ? desc_cache.bDeviceSubClass : 0; }
int strada_usb_desc_device_protocol_raw(void) { return desc_cache_valid ? desc_cache.bDeviceProtocol : 0; }
int strada_usb_desc_max_packet_size_raw(void) { return desc_cache_valid ? desc_cache.bMaxPacketSize0 : 0; }
int strada_usb_desc_vendor_id_raw(void) { return desc_cache_valid ? desc_cache.idVendor : 0; }
int strada_usb_desc_product_id_raw(void) { return desc_cache_valid ? desc_cache.idProduct : 0; }
int strada_usb_desc_device_version_raw(void) { return desc_cache_valid ? desc_cache.bcdDevice : 0; }
int strada_usb_desc_num_configs_raw(void) { return desc_cache_valid ? desc_cache.bNumConfigurations : 0; }
int strada_usb_desc_manufacturer_idx_raw(void) { return desc_cache_valid ? desc_cache.iManufacturer : 0; }
int strada_usb_desc_product_idx_raw(void) { return desc_cache_valid ? desc_cache.iProduct : 0; }
int strada_usb_desc_serial_idx_raw(void) { return desc_cache_valid ? desc_cache.iSerialNumber : 0; }

/* Get string descriptor */
char* strada_usb_get_string_descriptor_raw(void *handle, int idx) {
    if (!handle || idx == 0) return strdup("");
    unsigned char buffer[256];
    int ret = libusb_get_string_descriptor_ascii((libusb_device_handle *)handle, idx, buffer, sizeof(buffer));
    if (ret < 0) return strdup("");
    return strdup((char *)buffer);
}

/* Configuration */
int strada_usb_get_configuration_raw(void *handle) {
    if (!handle) return LIBUSB_ERROR_INVALID_PARAM;
    int config;
    int ret = libusb_get_configuration((libusb_device_handle *)handle, &config);
    if (ret < 0) return ret;
    return config;
}

int strada_usb_set_configuration_raw(void *handle, int config) {
    if (!handle) return LIBUSB_ERROR_INVALID_PARAM;
    return libusb_set_configuration((libusb_device_handle *)handle, config);
}

int strada_usb_set_interface_alt_setting_raw(void *handle, int iface, int alt) {
    if (!handle) return LIBUSB_ERROR_INVALID_PARAM;
    return libusb_set_interface_alt_setting((libusb_device_handle *)handle, iface, alt);
}

int strada_usb_clear_halt_raw(void *handle, int endpoint) {
    if (!handle) return LIBUSB_ERROR_INVALID_PARAM;
    return libusb_clear_halt((libusb_device_handle *)handle, (unsigned char)endpoint);
}

int strada_usb_reset_device_raw(void *handle) {
    if (!handle) return LIBUSB_ERROR_INVALID_PARAM;
    return libusb_reset_device((libusb_device_handle *)handle);
}

/* Debug level */
void strada_usb_set_debug_raw(int level) {
    if (!usb_initialized) strada_usb_init();
#if LIBUSB_API_VERSION >= 0x01000106
    libusb_set_option(usb_ctx, LIBUSB_OPTION_LOG_LEVEL, level);
#else
    libusb_set_debug(usb_ctx, level);
#endif
}

/* Error string */
const char* strada_usb_strerror_raw(int errcode) {
    return libusb_strerror(errcode);
}

/* Constants - return plain ints for extern C */
int strada_usb_class_hid_raw(void) { return LIBUSB_CLASS_HID; }
int strada_usb_class_mass_storage_raw(void) { return LIBUSB_CLASS_MASS_STORAGE; }
int strada_usb_class_hub_raw(void) { return LIBUSB_CLASS_HUB; }
int strada_usb_class_vendor_spec_raw(void) { return LIBUSB_CLASS_VENDOR_SPEC; }
int strada_usb_class_printer_raw(void) { return LIBUSB_CLASS_PRINTER; }
int strada_usb_class_audio_raw(void) { return LIBUSB_CLASS_AUDIO; }
int strada_usb_class_video_raw(void) { return LIBUSB_CLASS_VIDEO; }
int strada_usb_class_comm_raw(void) { return LIBUSB_CLASS_COMM; }

int strada_usb_request_type_standard_raw(void) { return LIBUSB_REQUEST_TYPE_STANDARD; }
int strada_usb_request_type_class_raw(void) { return LIBUSB_REQUEST_TYPE_CLASS; }
int strada_usb_request_type_vendor_raw(void) { return LIBUSB_REQUEST_TYPE_VENDOR; }
int strada_usb_recipient_device_raw(void) { return LIBUSB_RECIPIENT_DEVICE; }
int strada_usb_recipient_interface_raw(void) { return LIBUSB_RECIPIENT_INTERFACE; }
int strada_usb_recipient_endpoint_raw(void) { return LIBUSB_RECIPIENT_ENDPOINT; }
int strada_usb_endpoint_in_raw(void) { return LIBUSB_ENDPOINT_IN; }
int strada_usb_endpoint_out_raw(void) { return LIBUSB_ENDPOINT_OUT; }

int strada_usb_error_io_raw(void) { return LIBUSB_ERROR_IO; }
int strada_usb_error_invalid_param_raw(void) { return LIBUSB_ERROR_INVALID_PARAM; }
int strada_usb_error_access_raw(void) { return LIBUSB_ERROR_ACCESS; }
int strada_usb_error_no_device_raw(void) { return LIBUSB_ERROR_NO_DEVICE; }
int strada_usb_error_not_found_raw(void) { return LIBUSB_ERROR_NOT_FOUND; }
int strada_usb_error_busy_raw(void) { return LIBUSB_ERROR_BUSY; }
int strada_usb_error_timeout_raw(void) { return LIBUSB_ERROR_TIMEOUT; }
int strada_usb_error_overflow_raw(void) { return LIBUSB_ERROR_OVERFLOW; }
int strada_usb_error_pipe_raw(void) { return LIBUSB_ERROR_PIPE; }
int strada_usb_error_interrupted_raw(void) { return LIBUSB_ERROR_INTERRUPTED; }
int strada_usb_error_no_mem_raw(void) { return LIBUSB_ERROR_NO_MEM; }
int strada_usb_error_not_supported_raw(void) { return LIBUSB_ERROR_NOT_SUPPORTED; }
