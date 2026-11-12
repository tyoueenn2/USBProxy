#include "../Plugins/Hosts/GadgetFS_descriptors.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); } } while (0)

int main() {
	using namespace gadgetfs;
	CHECK(interrupt_interval(1, false, true) == 4); // 1000 Hz
	CHECK(interrupt_interval(2, false, true) == 5); // 500 Hz
	CHECK(interrupt_interval(8, false, true) == 7); // 125 Hz
	CHECK(interrupt_interval(16, false, true) == 8); // 62.5 Hz
	CHECK(interrupt_interval(10, false, true) == 7); // round down
	CHECK(interrupt_interval(0, false, true) == 0);
	CHECK(interrupt_interval(4, true, false) == 1);
	CHECK(interrupt_interval(1, true, false) == 1); // FS minimum
	CHECK(interrupt_interval(8, true, false) == 16);
	CHECK(interrupt_interval(16, true, false) == 255); // FS maximum
	CHECK(interrupt_interval(4, true, true) == 4);

	// Configuration, IAD, HID interface, HID descriptor, IN/OUT endpoints,
	// alternate setting, and a bulk interface with bInterval=0.
	std::vector<unsigned char> cfg = {
		9,2,0,0,2,1,0,0x80,50,
		8,11,0,1,3,0,0,0,
		9,4,0,0,2,3,0,0,0,
		9,0x21,0x11,1,0,1,0x22,50,0,
		7,5,0x81,3,8,0,1,
		7,5,0x01,3,8,0,8,
		9,4,0,1,1,3,0,0,0,
		7,5,0x82,3,8,0,16,
		9,4,1,0,1,8,0,0,0,
		7,5,0x83,2,64,0,0
	};
	auto native = cfg;
	CHECK(adapt_configuration(cfg.data(), cfg.size(), false, true));
	CHECK(cfg[41] == 4 && cfg[48] == 7 && cfg[64] == 8 && cfg[80] == 0);
	CHECK(native[41] == 1 && native[64] == 16); // source remains intact
	cfg = native;
	CHECK(adapt_configuration(cfg.data(), cfg.size(), false, false));
	CHECK(cfg == native); // no rate changes when source/target speed match
	// Native HS schedules are kept exactly, including sub-millisecond rates.
	const struct { unsigned hz; unsigned char interval; } rates[] = {
		{500, 5}, {1000, 4}, {2000, 3}, {4000, 2}, {8000, 1}
	};
	for (const auto& rate : rates) {
		cfg = native;
		cfg[41] = rate.interval;
		auto advertised = cfg;
		CHECK(adapt_configuration(advertised.data(), advertised.size(), true, true));
		CHECK(advertised == cfg);
		CHECK(8000u / (1u << (advertised[41] - 1)) == rate.hz);
		// Endpoint configuration must agree with the advertised configuration.
		usb_endpoint_descriptor ep = {};
		ep.bLength = USB_DT_ENDPOINT_SIZE;
		ep.bDescriptorType = USB_DT_ENDPOINT;
		ep.bEndpointAddress = 0x81;
		ep.bmAttributes = USB_ENDPOINT_XFER_INT;
		ep.bInterval = rate.interval;
		adapt_endpoint(&ep, true, true);
		CHECK(ep.bInterval == advertised[41]);
	}
	// All valid HS encodings, including slow endpoints, survive unchanged.
	for (unsigned char interval = 1; interval <= 16; ++interval)
		CHECK(interrupt_interval(interval, true, true) == interval);
	// FS mouse schedules map to the same elapsed time at HS.
	for (unsigned char interval : {1, 2, 4, 8, 16}) {
		cfg = native;
		cfg[41] = interval;
		CHECK(adapt_configuration(cfg.data(), cfg.size(), false, true));
		CHECK((1u << (cfg[41] - 1)) * 125 == unsigned(interval) * 1000);
	}
	unsigned char bad[] = {0, 5};
	CHECK(!adapt_configuration(bad, sizeof(bad), false, true));
	unsigned char truncated[] = {7, 5, 0x81};
	CHECK(!adapt_configuration(truncated, sizeof(truncated), false, true));
	unsigned char shortEp[] = {2, 5};
	CHECK(!adapt_configuration(shortEp, sizeof(shortEp), false, true));
	puts("descriptor tests passed");
}
