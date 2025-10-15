#include <iostream>
#include <iomanip>
#include <libusb-1.0/libusb.h>

int main() {
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0) {
        std::cerr << "libusb init failed\n";
        return 1;
    }

    libusb_device **list = nullptr;
    ssize_t cnt = libusb_get_device_list(ctx, &list);
    std::cout << "Devices found: " << cnt << "\n";

    for (ssize_t i = 0; i < cnt; ++i) {
        libusb_device *dev = list[i];
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(dev, &desc) == 0) {
            std::cout << "Bus " << int(libusb_get_bus_number(dev))
                      << " Dev " << int(libusb_get_device_address(dev))
                      << " VID:PID " << std::hex << std::setw(4) << std::setfill('0') << desc.idVendor
                      << ":" << std::setw(4) << desc.idProduct << std::dec << "\n";
        }
    }

    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return 0;
}