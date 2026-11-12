/* This file is part of USBProxy. */
#ifndef USBPROXY_REPORT_RATE_H
#define USBPROXY_REPORT_RATE_H

#include <chrono>
#include <stdio.h>

// One counter per endpoint, used only by that endpoint's transfer thread.
class ReportRate {
	bool enabled = false, started = false;
	unsigned count = 0;
	std::chrono::steady_clock::time_point start;
public:
	void enable(bool value) { enabled = value; }
	void record(const char* stage, unsigned endpoint) {
		if (!enabled) return;
		auto now = std::chrono::steady_clock::now();
		if (!started) { start = now; started = true; }
		++count;
		double seconds = std::chrono::duration<double>(now - start).count();
		if (seconds >= 1.0) {
			fprintf(stderr, "[rate] %s EP%02x: %.1f reports/s\n", stage, endpoint, count / seconds);
			count = 0;
			start = now;
		}
	}
};
#endif
