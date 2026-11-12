/* This file is part of USBProxy. */
#ifndef USBPROXY_GADGETFS_DESCRIPTORS_H
#define USBPROXY_GADGETFS_DESCRIPTORS_H

#include <stddef.h>
#include <stdint.h>
#include <linux/usb/ch9.h>

namespace gadgetfs {

// FS interrupt intervals use milliseconds; HS intervals use 2^(bInterval-1)
// microframes (125 us). Round down to avoid reducing the source polling rate.
inline uint8_t interrupt_interval(uint8_t interval, bool sourceHighSpeed,
                                  bool targetHighSpeed) {
	if (!interval || sourceHighSpeed == targetHighSpeed) return interval;
	if (targetHighSpeed) {
		unsigned microframes = unsigned(interval) * 8;
		uint8_t exponent = 1;
		while (microframes > 1) { microframes >>= 1; ++exponent; }
		return exponent;
	}
	if (interval > 16) interval = 16;
	unsigned milliseconds = (1u << (interval - 1)) / 8;
	return milliseconds < 1 ? 1 : milliseconds > 255 ? 255 : milliseconds;
}

inline void adapt_endpoint(usb_endpoint_descriptor* ep, bool sourceHighSpeed,
                           bool targetHighSpeed) {
	if ((ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) != USB_ENDPOINT_XFER_INT)
		return;
	ep->bInterval = interrupt_interval(ep->bInterval, sourceHighSpeed, targetHighSpeed);
}

// Walk the wire descriptors, including alternate settings and class/IAD
// descriptors, instead of assuming one interface descriptor per interface.
inline bool adapt_configuration(void* buffer, size_t length,
                                bool sourceHighSpeed, bool targetHighSpeed) {
	uint8_t* data = static_cast<uint8_t*>(buffer);
	for (size_t offset = 0; offset < length;) {
		if (length - offset < 2) return false;
		uint8_t size = data[offset], type = data[offset + 1];
		if (size < 2 || size > length - offset) return false;
		if (type == USB_DT_INTERFACE) {
			if (size < USB_DT_INTERFACE_SIZE) return false;
		} else if (type == USB_DT_ENDPOINT) {
			if (size < USB_DT_ENDPOINT_SIZE) return false;
			adapt_endpoint(reinterpret_cast<usb_endpoint_descriptor*>(data + offset),
			               sourceHighSpeed, targetHighSpeed);
		}
		offset += size;
	}
	return true;
}

} // namespace gadgetfs
#endif
