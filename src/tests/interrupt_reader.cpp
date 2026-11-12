// Deterministic fake transport: exercise the production reader without USB hardware.
#include "../Plugins/Devices/InterruptReader.h"
#include <algorithm>
#include <thread>
#include <cstdio>
#include <future>
#include <memory>
#include <string>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); abort(); } } while (0)

static std::mutex transportMutex;
static std::condition_variable cancelledReady;
static std::deque<libusb_transfer*> submitted, cancelled;
static bool quit = false;
static int allocations = 0, failSubmission = 0, submissions = 0;
static int allocationCalls = 0, failAllocation = 0;

extern "C" {
libusb_transfer* LIBUSB_CALL libusb_alloc_transfer(int) {
	if (++allocationCalls == failAllocation) return nullptr;
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

static void regressions() {
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
	// Failure during allocation must release the earlier transfer buffers.
	for (int failure = 1; failure <= 8; ++failure) {
		failAllocation = allocationCalls + failure;
		{
			InterruptReader reader(nullptr, 0x81, 8, false);
			CHECK(reader.receive(&data, &length, 1) == LIBUSB_ERROR_NO_MEM);
			CHECK(data == nullptr && length == 0 && pending() == 0);
		}
		CHECK(allocations == 0);
	}
	failAllocation = 0;
	failSubmission = 0;
	for (auto status : {LIBUSB_TRANSFER_STALL, LIBUSB_TRANSFER_OVERFLOW, LIBUSB_TRANSFER_ERROR}) {
		InterruptReader reader(nullptr, 0x81, 8, false);
		complete(11); complete(12);
		complete(0, status, 0);
		for (int value : {11, 12}) {
			CHECK(reader.receive(&data, &length, 1) == 0 && length == 1 && data[0] == value);
			free(data);
		}
		int expected = status == LIBUSB_TRANSFER_STALL ? LIBUSB_ERROR_PIPE :
		               status == LIBUSB_TRANSFER_OVERFLOW ? LIBUSB_ERROR_OVERFLOW : LIBUSB_ERROR_IO;
		CHECK(reader.receive(&data, &length, 1) == expected && !data && !length);
	}
	CHECK(pending() == 0 && allocations == 0);
}

// Synthetic timestamps describe source cadence. This checks loss/order/content
// with concurrent producer/consumer threads, not wall-clock USB throughput.
static std::vector<unsigned char> payload(unsigned sequence, unsigned rate, unsigned char endpoint) {
	unsigned timestamp = sequence * (1000000 / rate);
	return {endpoint, static_cast<unsigned char>(sequence), static_cast<unsigned char>(sequence >> 8),
	        static_cast<unsigned char>(sequence >> 16), static_cast<unsigned char>(timestamp),
	        static_cast<unsigned char>(timestamp >> 8), static_cast<unsigned char>(timestamp >> 16),
	        static_cast<unsigned char>(timestamp >> 24)};
}

static void complete_report(unsigned char endpoint, const std::vector<unsigned char>& report) {
	libusb_transfer* transfer;
	{
		std::lock_guard<std::mutex> lock(transportMutex);
		auto it = std::find_if(submitted.begin(), submitted.end(), [endpoint](libusb_transfer* t) {
			return t->endpoint == endpoint;
		});
		CHECK(it != submitted.end());
		transfer = *it;
		submitted.erase(it);
	}
	CHECK(report.size() <= static_cast<size_t>(transfer->length));
	memcpy(transfer->buffer, report.data(), report.size());
	transfer->actual_length = report.size();
	transfer->status = LIBUSB_TRANSFER_COMPLETED;
	transfer->callback(transfer);
}

static void stream(unsigned rate, bool twoEndpoints = false) {
	CHECK(rate == 500 || rate == 1000 || rate == 8000);
	InterruptReader first(nullptr, 0x81, 8, false);
	std::unique_ptr<InterruptReader> second;
	if (twoEndpoints) second.reset(new InterruptReader(nullptr, 0x82, 8, false));
	CHECK(pending() == (twoEndpoints ? 16 : 8));
	const unsigned samples = rate * 2;
	unsigned consumed[2] = {0, 0};
	std::mutex progress;
	std::condition_variable advanced;
	auto consume = [&](InterruptReader& reader, unsigned index, unsigned char ep) {
		for (unsigned i = 0; i < samples; ++i) {
			unsigned char* data = nullptr;
			int length = 0;
			CHECK(reader.receive(&data, &length, 2000) == 0 && length == 8);
			auto expected = payload(i, rate, ep);
			CHECK(memcmp(data, expected.data(), expected.size()) == 0);
			free(data);
			{
				std::lock_guard<std::mutex> lock(progress);
				consumed[index] = i + 1;
			}
			advanced.notify_one();
		}
	};
	std::thread consumer(consume, std::ref(first), 0, 0x81);
	std::thread other;
	if (second) other = std::thread(consume, std::ref(*second), 1, 0x82);
	for (unsigned i = 0; i < samples; ++i) {
		// Bound test-producer bursts so the intended overload test remains
		// separate from report-preservation tests. There is no rate sleep.
		{
			std::unique_lock<std::mutex> lock(progress);
			advanced.wait(lock, [&] { return i < consumed[0] + 128 && (!second || i < consumed[1] + 128); });
		}
		complete_report(0x81, payload(i, rate, 0x81));
		if (second) complete_report(0x82, payload(i, rate, 0x82));
	}
	consumer.join();
	if (other.joinable()) other.join();
	CHECK(consumed[0] == samples && (!second || consumed[1] == samples));
	CHECK(pending() == (twoEndpoints ? 16 : 8));
	printf("Preserved %u synthetic %u Hz reports per endpoint in order\n", samples, rate);
}

static void stop_wakeup() {
	InterruptReader reader(nullptr, 0x81, 8, false);
	std::promise<void> started;
	auto result = std::async(std::launch::async, [&] {
		unsigned char* data = nullptr;
		int length = 0;
		started.set_value();
		int rc = reader.receive(&data, &length, 0);
		CHECK(data == nullptr && length == 0);
		return rc;
	});
	started.get_future().wait();
	CHECK(result.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
	reader.stop();
	CHECK(result.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
	CHECK(result.get() == LIBUSB_ERROR_INTERRUPTED);
	reader.stop(); // idempotent cancellation
	CHECK(pending() == 0);
}

int main(int argc, char** argv) {
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
	if (argc == 1) regressions();
	else if (argc == 3 && std::string(argv[1]) == "stream") stream(std::stoul(argv[2]));
	else if (argc == 2 && std::string(argv[1]) == "endpoints") stream(8000, true);
	else if (argc == 2 && std::string(argv[1]) == "stop") stop_wakeup();
	else CHECK(false);
	CHECK(pending() == 0 && allocations == 0);
	{
		std::lock_guard<std::mutex> lock(transportMutex);
		quit = true;
		cancelledReady.notify_one();
	}
	events.join();
	puts("interrupt reader tests passed");
}
