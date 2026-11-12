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
	CHECK(adapt_configuration(cfg.data(), cfg.size(), false, true, false));
	CHECK(cfg[41] == 4 && cfg[48] == 7 && cfg[64] == 8 && cfg[80] == 0);
	CHECK(native[41] == 1 && native[64] == 16); // source remains intact
	cfg = native;
	CHECK(adapt_configuration(cfg.data(), cfg.size(), false, true, true));
	CHECK(cfg[41] == 4 && cfg[48] == 7 && cfg[64] == 4 && cfg[80] == 0);
	cfg = native;
	CHECK(adapt_configuration(cfg.data(), cfg.size(), false, false, true));
	CHECK(cfg[41] == 1 && cfg[48] == 8 && cfg[64] == 1 && cfg[80] == 0);
	// Pre-existing HS descriptors must not be converted a second time.
	cfg[41] = 4;
	CHECK(adapt_configuration(cfg.data(), cfg.size(), true, true, false));
	CHECK(cfg[41] == 4);
	unsigned char bad[] = {0, 5};
	CHECK(!adapt_configuration(bad, sizeof(bad), false, true, false));
	unsigned char truncated[] = {7, 5, 0x81};
	CHECK(!adapt_configuration(truncated, sizeof(truncated), false, true, false));
	unsigned char shortEp[] = {2, 5};
	CHECK(!adapt_configuration(shortEp, sizeof(shortEp), false, true, false));
	puts("descriptor tests passed");
}
