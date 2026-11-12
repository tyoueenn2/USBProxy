// Deterministic fake transport: exercise the production reader without USB hardware.
#include "../Plugins/Devices/InterruptReader.h"
#include <algorithm>
#include <thread>
#include <cstdio>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); abort(); } } while (0)

static std::mutex transportMutex;
static std::condition_variable cancelledReady;
static std::deque<libusb_transfer*> submitted, cancelled;
static bool quit = false;
static int allocations = 0, failSubmission = 0, submissions = 0;

extern "C" {
libusb_transfer* LIBUSB_CALL libusb_alloc_transfer(int) {
	++allocations;
	return static_cast<libusb_transfer*>(calloc(1, sizeof(libusb_transfer)));
}
void LIBUSB_CALL libusb_free_transfer(libusb_transfer* transfer) {
	--allocations;
	free(transfer);
}
int LIBUSB_CALL libusb_submit_transfer(libusb_transfer* transfer) {
	std::lock_guard<std::mutex> lock(transportMutex);
	if (++submissions == failSubmission) return LIBUSB_ERROR_IO;
	submitted.push_back(transfer);
	return 0;
}
int LIBUSB_CALL libusb_cancel_transfer(libusb_transfer* transfer) {
	std::lock_guard<std::mutex> lock(transportMutex);
	auto it = std::find(submitted.begin(), submitted.end(), transfer);
	if (it == submitted.end()) return LIBUSB_ERROR_NOT_FOUND;
	submitted.erase(it);
	cancelled.push_back(transfer);
	cancelledReady.notify_one();
	return 0;
}
}

static size_t pending() {
	std::lock_guard<std::mutex> lock(transportMutex);
	return submitted.size();
}

static void complete(unsigned char value, libusb_transfer_status status = LIBUSB_TRANSFER_COMPLETED, int length = 1) {
	libusb_transfer* transfer;
	{
		std::lock_guard<std::mutex> lock(transportMutex);
		CHECK(!submitted.empty());
		transfer = submitted.front();
		submitted.pop_front();
	}
	transfer->status = status;
	transfer->actual_length = length;
	transfer->buffer[0] = value;
	transfer->callback(transfer);
}

int main() {
	std::thread events([] {
		std::unique_lock<std::mutex> lock(transportMutex);
		while (true) {
			cancelledReady.wait(lock, [] { return quit || !cancelled.empty(); });
			if (quit && cancelled.empty()) break;
			auto transfer = cancelled.front();
			cancelled.pop_front();
			lock.unlock();
			transfer->status = LIBUSB_TRANSFER_CANCELLED;
			transfer->callback(transfer);
			lock.lock();
		}
	});
	unsigned char* data;
	int length;
	{
		InterruptReader reader(nullptr, 0x81, 8, false);
		CHECK(pending() == 8);
		CHECK(reader.receive(&data, &length, 1) == LIBUSB_ERROR_TIMEOUT);
		CHECK(data == nullptr && length == 0 && pending() == 8);
		complete(1); complete(2); // rearmed before the relay consumes either report
		CHECK(pending() == 8);
		for (int value = 1; value <= 2; ++value) {
			CHECK(reader.receive(&data, &length, 1) == 0);
			CHECK(length == 1 && data[0] == value);
			free(data);
		}
		complete(0, LIBUSB_TRANSFER_COMPLETED, 0); // preserve ZLP semantics
		CHECK(reader.receive(&data, &length, 1) == 0 && data && length == 0);
		free(data);
		complete(0, LIBUSB_TRANSFER_TIMED_OUT, 0);
		CHECK(pending() == 8);
		for (int i = 0; i < 2000; ++i) {
			complete(i & 255);
			CHECK(reader.receive(&data, &length, 1) == 0 && data[0] == (i & 255));
			free(data);
		}
		reader.stop();
		CHECK(reader.receive(&data, &length, 1) == LIBUSB_ERROR_INTERRUPTED);
	}
	CHECK(pending() == 0 && allocations == 0);
	{
		InterruptReader reader(nullptr, 0x81, 8, false);
		complete(0, LIBUSB_TRANSFER_NO_DEVICE, 0);
		CHECK(reader.receive(&data, &length, 1) == LIBUSB_ERROR_NO_DEVICE);
	}
	{
		InterruptReader reader(nullptr, 0x81, 8, false);
		for (int i = 0; i < 257; ++i) complete(i & 255);
		for (int i = 0; i < 256; ++i) {
			CHECK(reader.receive(&data, &length, 1) == 0 && data[0] == (i & 255));
			free(data);
		}
		CHECK(reader.receive(&data, &length, 1) == LIBUSB_ERROR_OVERFLOW);
	}
	failSubmission = submissions + 3;
	{
		InterruptReader reader(nullptr, 0x81, 8, false);
		CHECK(pending() == 2);
		CHECK(reader.receive(&data, &length, 1) == LIBUSB_ERROR_IO);
	}
	failSubmission = 0;
	CHECK(pending() == 0 && allocations == 0);
	{
		InterruptReader reader(nullptr, 0x81, 8, false);
		failSubmission = submissions + 1;
		complete(42); // callback rearm fails, but deliver the completed report
		CHECK(reader.receive(&data, &length, 1) == 0 && data[0] == 42);
		free(data);
		CHECK(reader.receive(&data, &length, 1) == LIBUSB_ERROR_IO);
	}
	CHECK(pending() == 0 && allocations == 0);
	{
		std::lock_guard<std::mutex> lock(transportMutex);
		quit = true;
		cancelledReady.notify_one();
	}
	events.join();
	puts("interrupt reader tests passed");
}
