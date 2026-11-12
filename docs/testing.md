# Testing USBProxy mouse forwarding

## Run the automated tests

Install the build dependencies described in [mouse-polling.md](mouse-polling.md),
including Python 3 for CLI checks, then run:

```sh
cmake -S src -B src/build -DBUILD_GREENGRASS=OFF -DBUILD_TESTING=ON
cmake --build src/build -j4
ctest --test-dir src/build --output-on-failure
```

All nine tests run without USB hardware, root permissions, a mounted GadgetFS
filesystem, or a Windows host. Use `ctest --test-dir src/build -N` to list them.
Individual failures include the failed check and source line. Each test has a
timeout so a broken cancellation or producer/consumer path fails instead of
hanging CI indefinitely.

| Test | What it checks |
| --- | --- |
| `gadgetfs-descriptors` | Native 500/1000/2000/4000/8000 Hz HS schedules; all FS interval encodings; non-interrupt descriptors; alternate interfaces; malformed lengths. |
| `gadgetfs-enumeration-endpoints` | Production device-model enumeration and GadgetFS endpoint setup agree for FS/HS sources, with/without an other-speed configuration. Composite interfaces have distinct intervals, and the source descriptors remain unchanged. Gadget filesystem calls are replaced with an in-memory capture. |
| `cli-options` | Built executable's help and stats options work; unknown options fail; a fixed 1000 Hz option is absent. |
| `interrupt-reader` | Ordered reports, ZLPs, idle timeouts, queue overflow, unplug, stall/transport errors, allocation failure at every transfer slot, initial submission and rearm failures, and cleanup. Buffered reports arrive before a later terminal error. |
| `interrupt-stream-500` | Concurrent forwarding of 1000 reports with synthetic 500 Hz timestamps, byte-for-byte and in order. |
| `interrupt-stream-1000` | Concurrent forwarding of 2000 reports with synthetic 1000 Hz timestamps. |
| `interrupt-stream-8000` | Concurrent forwarding of 16000 reports with synthetic 8000 Hz timestamps. |
| `interrupt-endpoint-isolation` | Two concurrent endpoints each preserve 16000 reports without mixing their payloads or order. |
| `interrupt-stop-wakeup` | Stopping wakes an infinite receive wait, drains transfer cancellations and can be called repeatedly. |

The synthetic stream tests run as fast as the test machine allows. Their source
timestamps identify the input sequence and cadence; they do not measure sustained
USB throughput or prove that a Pi can forward 8000 reports/s. Producer bursts are
bounded below the overflow threshold to isolate preservation/concurrency from the
separate overload test. No report-rate setting is introduced in USBProxy.

Run only a group using its CTest label, for example:

```sh
ctest --test-dir src/build -L concurrency --output-on-failure
```

## Continuous integration

The `tests` GitHub Actions workflow runs on pull requests and pushes to `master`.
It builds on Linux x86-64 and ARM64, plus an x86-64 build with AddressSanitizer
and UndefinedBehaviorSanitizer. It installs public distro packages and requires
no Nesto credentials or AWS SDK. JUnit results and the CTest log are saved as
artifacts for every job. The workflow has read-only repository permissions.

To run a sanitizer build locally with GCC or Clang:

```sh
CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
CXXFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
cmake -S src -B /tmp/usbproxy-sanitized \
    -DBUILD_GREENGRASS=OFF -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/usbproxy-sanitized -j4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir /tmp/usbproxy-sanitized --output-on-failure
```

LeakSanitizer requires an environment where it can inspect the process. The
managed workspace runs these tests with `detect_leaks=0` because of its ptrace
sandbox; the GitHub sanitizer job keeps leak checking enabled.

## Validate the real Pi/Windows path

Software tests complement this hardware procedure:

1. Record the Pi model, OS/kernel (`uname -a`), mouse model and onboard profile.
2. Measure the mouse connected directly to Windows using a native Raw Input
   polling tool. Record its rate during continuous movement. Repeat with the
   browser tool if that was used to observe the original slowdown.
3. Connect through the Pi using the same mouse profile. Capture physical device
   details with `lsusb -t` and `sudo lsusb -v -d VID:PID`. Check the gadget's
   negotiated speed and interrupt interval on Windows with USBView or USB Device
   Tree Viewer. An 8000 Hz schedule needs high-speed USB and `bInterval=1`.
4. Build/install the tested commit and run USBProxy with the mouse's hexadecimal
   IDs and `--stats`, without packet loggers or verbose debugging:
   `sudo /usr/local/bin/usb-mitm -v VENDOR_ID -p PRODUCT_ID --stats`.
5. Move continuously for at least ten seconds. Compare steady `mouse->Pi` and
   `Pi->host` rate lines for the same endpoint with the Windows native tool.
   Exclude initial idle intervals. Check buttons/wheel as well as movement, and
   verify that no overflow or transfer-error messages occur.
6. Repeat for each profile the mouse actually supports (for example 500, 1000
   and 8000 Hz). Set the profile in the mouse, then reconnect as needed. Do not
   change or emulate its rate in the proxy.

Record measurements against the exact commit:

| Profile | Direct Windows rate | Pi input | Completed gadget output | Windows through Pi | USB speed / bInterval | Errors |
| --- | --- | --- | --- | --- | --- | --- |
| 500 Hz | | | | | | |
| 1000 Hz | | | | | | |
| 8000 Hz, if supported | | | | | | |

A slow Pi input points to the mouse/profile or Pi host side. Fast input with slow
gadget output points to the forwarding/gadget/Windows side. Matching counters
with a browser-only slowdown calls for a native input measurement or USB trace,
since browser event delivery may be frame-limited. Keep raw rate lines and logs
with the table rather than recording an assumed rate from the descriptor alone.
