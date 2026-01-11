# Strada USB Library

Userspace USB device access for Strada via libusb-1.0.

## Requirements

Install libusb development headers:

```bash
# Ubuntu/Debian
sudo apt install libusb-1.0-0-dev

# RHEL/CentOS
sudo yum install libusb1-devel

# macOS
brew install libusb
```

## Building

```bash
cd lib/usb
make
```

## Usage

```strada
use lib "lib";
use usb;

# Initialize
usb::init();

# List devices
my array @devices = usb::get_device_list();
foreach my hash %dev (@devices) {
    say("Found: " . $dev{"vidpid"});
}

# Open a device
my int $handle = usb::open_device(0x1234, 0x5678);
if ($handle != 0) {
    # Claim interface
    usb::set_auto_detach($handle, 1);
    usb::claim_interface($handle, 0);

    # Bulk write
    my int $written = usb::bulk_transfer($handle, 0x01, "Hello", 1000);

    # Bulk read
    my str $data = usb::bulk_transfer($handle, 0x81, 64, 1000);

    # Cleanup
    usb::release_interface($handle, 0);
    usb::close($handle);
}

usb::exit();
```

## Running the Example

```bash
# From strada root directory
./strada -r lib/usb/example.strada

# List all devices
./strada -r lib/usb/example.strada list

# Show device details (VID:PID in hex)
./strada -r lib/usb/example.strada info 046d c52b

# Demo control transfer
./strada -r lib/usb/example.strada status 046d c52b
```

## Permissions

To access USB devices without root, add a udev rule:

```bash
# /etc/udev/rules.d/99-usb.rules
SUBSYSTEM=="usb", MODE="0666"

# Or for a specific device:
SUBSYSTEM=="usb", ATTR{idVendor}=="1234", ATTR{idProduct}=="5678", MODE="0666"
```

Then reload: `sudo udevadm control --reload-rules && sudo udevadm trigger`

## API Reference

### Initialization
- `usb::init()` - Initialize libusb (called automatically)
- `usb::exit()` - Cleanup libusb
- `usb::set_debug(level)` - Set debug level (0-4)
- `usb::strerror(code)` - Get error message

### Device Enumeration
- `usb::get_device_list()` - Returns array of device hashes

### Device Open/Close
- `usb::open_device(vid, pid)` - Open by Vendor/Product ID
- `usb::open_device_by_path(bus, addr)` - Open by bus location
- `usb::close(handle)` - Close device

### Interface Management
- `usb::claim_interface(handle, iface)` - Claim interface
- `usb::release_interface(handle, iface)` - Release interface
- `usb::detach_kernel_driver(handle, iface)` - Detach kernel driver
- `usb::kernel_driver_active(handle, iface)` - Check kernel driver
- `usb::set_auto_detach(handle, enable)` - Auto-detach mode

### Data Transfers
- `usb::bulk_transfer(handle, endpoint, data, timeout)`
- `usb::interrupt_transfer(handle, endpoint, data, timeout)`
- `usb::control_transfer(handle, req_type, req, value, index, data, timeout)`

### Descriptors
- `usb::get_device_descriptor(handle)` - Device info hash
- `usb::get_string_descriptor(handle, index)` - String descriptor
- `usb::get_config_descriptor(handle, index)` - Configuration info

### Configuration
- `usb::get_configuration(handle)` - Get active config
- `usb::set_configuration(handle, config)` - Set config
- `usb::set_interface_alt_setting(handle, iface, alt)`
- `usb::clear_halt(handle, endpoint)` - Clear stall
- `usb::reset_device(handle)` - Reset device

### Constants
- Class codes: `CLASS_HID()`, `CLASS_MASS_STORAGE()`, `CLASS_HUB()`, etc.
- Endpoints: `ENDPOINT_IN()`, `ENDPOINT_OUT()`
- Request types: `REQUEST_TYPE_STANDARD()`, `REQUEST_TYPE_CLASS()`, `REQUEST_TYPE_VENDOR()`
- Error codes: `ERROR_IO()`, `ERROR_ACCESS()`, `ERROR_TIMEOUT()`, etc.
