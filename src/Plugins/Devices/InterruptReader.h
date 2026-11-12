/* This file is part of USBProxy. */
#ifndef USBPROXY_INTERRUPT_READER_H
#define USBPROXY_INTERRUPT_READER_H

#include <libusb-1.0/libusb.h>
#include <array>
#include <chrono>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include "ReportRate.h"

// Keep interrupt IN URBs pending while the relay thread forwards earlier
// reports. The owning DeviceProxy must keep pumping libusb events until stop()
// has drained cancellation callbacks, and must stop before closing the handle.
class InterruptReader {
	std::array<libusb_transfer*, 8> transfers = {{}};
	std::deque<std::vector<unsigned char>> reports;
	std::mutex mutex;
	std::condition_variable changed;
	unsigned pending = 0;
	bool stopping = false;
	int error = 0;
	ReportRate rate;

	static void LIBUSB_CALL completed(libusb_transfer* transfer) {
		static_cast<InterruptReader*>(transfer->user_data)->complete(transfer);
	}
	void complete(libusb_transfer* transfer) {
		std::lock_guard<std::mutex> lock(mutex);
		--pending;
		if (!stopping && !error) {
			if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
				// Fail visibly instead of silently losing mouse/button reports if
				// the host cannot keep up. The bound is a report count, not a rate.
				if (reports.size() >= 256) error = LIBUSB_ERROR_OVERFLOW;
				else {
					reports.emplace_back(transfer->buffer, transfer->buffer + transfer->actual_length);
					if (transfer->actual_length) rate.record("mouse->Pi", transfer->endpoint);
				}
			} else if (transfer->status != LIBUSB_TRANSFER_TIMED_OUT) {
				switch (transfer->status) {
				case LIBUSB_TRANSFER_NO_DEVICE: error = LIBUSB_ERROR_NO_DEVICE; break;
				case LIBUSB_TRANSFER_STALL: error = LIBUSB_ERROR_PIPE; break;
				case LIBUSB_TRANSFER_OVERFLOW: error = LIBUSB_ERROR_OVERFLOW; break;
				default: error = LIBUSB_ERROR_IO; break;
				}
			}
			if (!error) {
				int rc = libusb_submit_transfer(transfer);
				if (rc) error = rc;
				else ++pending;
			}
		}
		changed.notify_all();
	}

public:
	InterruptReader(libusb_device_handle* handle, unsigned char endpoint, int packetSize, bool stats) {
		rate.enable(stats);
		std::lock_guard<std::mutex> lock(mutex);
		for (auto& transfer : transfers) {
			transfer = libusb_alloc_transfer(0);
			unsigned char* buffer = static_cast<unsigned char*>(malloc(packetSize));
			if (!transfer || !buffer) { free(buffer); error = LIBUSB_ERROR_NO_MEM; break; }
			libusb_fill_interrupt_transfer(transfer, handle, endpoint, buffer, packetSize, completed, this, 0);
		}
		if (!error) {
			for (auto transfer : transfers) {
				int rc = libusb_submit_transfer(transfer);
				if (rc) { error = rc; break; }
				++pending;
			}
		}
	}
	~InterruptReader() {
		stop();
		for (auto transfer : transfers) {
			if (transfer) { free(transfer->buffer); libusb_free_transfer(transfer); }
		}
	}
	void stop() {
		std::unique_lock<std::mutex> lock(mutex);
		if (!stopping) {
			stopping = true;
			for (auto transfer : transfers)
				if (transfer) libusb_cancel_transfer(transfer);
			changed.notify_all();
		}
		changed.wait(lock, [this] { return pending == 0; });
	}
	int receive(unsigned char** data, int* length, int timeout) {
		*data = nullptr;
		*length = 0;
		std::unique_lock<std::mutex> lock(mutex);
		auto ready = [this] { return !reports.empty() || error || stopping; };
		if (timeout > 0) {
			if (!changed.wait_for(lock, std::chrono::milliseconds(timeout), ready))
				return LIBUSB_ERROR_TIMEOUT;
		} else changed.wait(lock, ready);
		if (reports.empty()) return error ? error : LIBUSB_ERROR_INTERRUPTED;
		const auto& report = reports.front();
		*data = static_cast<unsigned char*>(malloc(report.empty() ? 1 : report.size()));
		if (!*data) return LIBUSB_ERROR_NO_MEM;
		if (!report.empty()) memcpy(*data, report.data(), report.size());
		*length = report.size();
		reports.pop_front();
		return 0;
	}
};
#endif
