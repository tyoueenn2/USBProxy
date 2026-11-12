// Exercise production enumeration and endpoint setup without mounting GadgetFS.
#include "../Plugins/Hosts/HostProxy_GadgetFS.h"
#include "../Plugins/Hosts/GadgetFS_helpers.h"
#include "Device.h"
#include "DeviceProxy.h"
#include "DeviceQualifier.h"
#include "Configuration.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <zmq.hpp>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); abort(); } } while (0)

zmq::context_t* ctx = nullptr; // required by libUSBProxy; relays are not started
static const int gadgetFd = 10000;
static std::map<int, std::vector<unsigned char>> writes;
static unsigned mounts = 0, unmounts = 0;

extern "C" {
int mount_gadget() { ++mounts; return 0; }
int unmount_gadget() { ++unmounts; return 0; }
const char* find_gadget_filename() { return "test_udc"; }
int open_gadget(const char*) { return gadgetFd; }
int open_endpoint(__u8 ep, const char*) { return gadgetFd + ep; }
ssize_t __real_write(int, const void*, size_t);
ssize_t __wrap_write(int fd, const void* data, size_t length) {
	if (fd < gadgetFd) return __real_write(fd, data, length);
	const auto* bytes = static_cast<const unsigned char*>(data);
	writes[fd] = std::vector<unsigned char>(bytes, bytes + length);
	return length;
}
int __real_close(int);
int __wrap_close(int fd) { return fd >= gadgetFd ? 0 : __real_close(fd); }
int __wrap_aio_cancel(int fd, aiocb*) {
	CHECK(fd >= gadgetFd);
	return AIO_ALLDONE;
}
}

// A real device model is populated through the normal control-request API.
// Interface 1 and its alternate deliberately have different intervals from
// interface 0, catching the old other-speed interface-index mixup.
static std::vector<unsigned char> configuration(bool hs, unsigned char interval) {
	std::vector<unsigned char> data = {
		9, USB_DT_CONFIG, 57, 0, 2, 1, 0, 0x80, 50,
		9, USB_DT_INTERFACE, 0, 0, 1, USB_CLASS_HID, 0, 0, 0,
		7, USB_DT_ENDPOINT, 0x81, USB_ENDPOINT_XFER_INT, 8, 0, interval,
		9, USB_DT_INTERFACE, 1, 0, 1, USB_CLASS_HID, 0, 0, 0,
		7, USB_DT_ENDPOINT, 0x82, USB_ENDPOINT_XFER_INT, 8, 0, static_cast<unsigned char>(hs ? 5 : 2),
		9, USB_DT_INTERFACE, 1, 1, 1, USB_CLASS_HID, 0, 0, 0,
		7, USB_DT_ENDPOINT, 0x83, USB_ENDPOINT_XFER_INT, 8, 0, static_cast<unsigned char>(hs ? 7 : 8)
	};
	CHECK(data.size() == 57);
	return data;
}

class FakeDevice : public DeviceProxy {
	bool highSpeed, otherSpeed;
	std::vector<unsigned char> native, other;
public:
	FakeDevice(bool hs, bool qualifier, unsigned char nativeInterval, unsigned char otherInterval)
		: highSpeed(hs), otherSpeed(qualifier), native(configuration(hs, nativeInterval)),
		  other(configuration(!hs, otherInterval)) { other[1] = USB_DT_OTHER_SPEED_CONFIG; }
	int control_request(const usb_ctrlrequest* request, int* length, __u8* data, int) override {
		std::vector<unsigned char> reply;
		if (request->bRequest == USB_REQ_GET_INTERFACE) reply = {0};
		else if (request->bRequest == USB_REQ_GET_DESCRIPTOR) {
			switch (request->wValue >> 8) {
			case USB_DT_DEVICE: reply = {18, USB_DT_DEVICE, 0, 2, 0, 0, 0, 64, 0x34, 0x12, 0x78, 0x56, 0, 1, 0, 0, 0, 1}; break;
			case USB_DT_STRING: reply = {4, USB_DT_STRING, 9, 4}; break;
			case USB_DT_CONFIG: reply = native; break;
			case USB_DT_OTHER_SPEED_CONFIG: reply = other; break;
			case USB_DT_DEVICE_QUALIFIER:
				if (!otherSpeed) return -1;
				reply = {10, USB_DT_DEVICE_QUALIFIER, 0, 2, 0, 0, 0, 64, 1, 0}; break;
			default: CHECK(false);
			}
		} else CHECK(false);
		*length = std::min<size_t>(reply.size(), request->wLength);
		memcpy(data, reply.data(), *length);
		return 0;
	}
	int connect(int) override { return 0; }
	void disconnect() override {}
	void reset() override {}
	bool is_connected() override { return true; }
	bool is_highspeed() override { return highSpeed; }
	void send_data(__u8, __u8, __u16, __u8*, int) override { CHECK(false); }
	void receive_data(__u8, __u8, __u16, __u8**, int*, int) override { CHECK(false); }
	void setConfig(Configuration*, Configuration*, bool) override {}
	void set_endpoint_interface(__u8, __u8) override {}
	void claim_interface(__u8) override {}
	void release_interface(__u8) override {}
	__u8 get_address() override { return 0; }
};

static std::map<unsigned char, unsigned char> intervals(const std::vector<unsigned char>& data) {
	std::map<unsigned char, unsigned char> result;
	for (size_t offset = 0; offset < data.size(); offset += data[offset]) {
		CHECK(data[offset] >= 2 && offset + data[offset] <= data.size());
		if (data[offset + 1] == USB_DT_ENDPOINT) result[data[offset + 2]] = data[offset + 6];
	}
	return result;
}

static void run(bool hs, bool qualifier, unsigned char nativeInterval,
                unsigned char otherInterval, unsigned char expectedFs, unsigned char expectedHs) {
	writes.clear();
	FakeDevice source(hs, qualifier, nativeInterval, otherInterval);
	Device device(&source);
	ConfigParser cfg;
	{
		HostProxy_GadgetFS host(&cfg);
		auto nativeBefore = device.get_configuration(1)->get_full_descriptor();
		std::vector<unsigned char> nativeCopy(nativeBefore, nativeBefore + device.get_configuration(1)->get_full_descriptor_length());
		free(nativeBefore);
		CHECK(host.connect(&device) == 0);
		auto& descriptor = writes.at(gadgetFd);
		CHECK(descriptor.size() >= 4 + 2 * 57 + 18);
		std::vector<unsigned char> fs(descriptor.begin() + 4, descriptor.begin() + 4 + 57);
		std::vector<unsigned char> high(descriptor.begin() + 4 + 57, descriptor.begin() + 4 + 2 * 57);
		auto fsIntervals = intervals(fs), hsIntervals = intervals(high);
		CHECK(fsIntervals.at(0x81) == expectedFs && hsIntervals.at(0x81) == expectedHs);
		CHECK(fsIntervals.at(0x82) == 2 && hsIntervals.at(0x82) == 5);
		CHECK(fsIntervals.at(0x83) == 8 && hsIntervals.at(0x83) == 7);
		Configuration* native = device.get_configuration(1);
		Configuration* other = qualifier ? device.get_device_qualifier()->get_configuration(1) : nullptr;
		if (other) host.setConfig(hs ? other : native, hs ? native : other, hs);
		else host.setConfig(native, nullptr, hs);
		for (unsigned char ep : {0x81, 0x82, 0x83}) {
			const auto& endpoint = writes.at(gadgetFd + ep);
			CHECK(endpoint.size() == 18 && endpoint[0] == 1);
			CHECK(endpoint[4 + 6] == fsIntervals.at(ep));
			CHECK(endpoint[4 + 7 + 6] == hsIntervals.at(ep));
		}
		auto nativeAfter = native->get_full_descriptor();
		CHECK(memcmp(nativeCopy.data(), nativeAfter, nativeCopy.size()) == 0);
		free(nativeAfter);
	}
	CHECK(mounts == unmounts);
}

int main() {
	run(false, false, 1, 4, 1, 4); // FS 1000 Hz
	run(false, false, 2, 5, 2, 5); // FS 500 Hz
	run(false, true, 1, 4, 1, 4); // use native other-speed configuration
	for (unsigned char hsInterval : {1, 2, 3, 4, 5}) {
		unsigned char fsInterval = hsInterval == 5 ? 2 : 1;
		run(true, true, hsInterval, fsInterval, fsInterval, hsInterval);
		run(true, false, hsInterval, fsInterval, fsInterval, hsInterval);
	}
	puts("GadgetFS enumeration/endpoint integration tests passed (500-8000 Hz)");
}
