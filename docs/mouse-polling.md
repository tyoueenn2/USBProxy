# 1000 Hz mouse forwarding on Raspberry Pi 4

This fork adds queued interrupt IN reads, consistent GadgetFS polling intervals,
and an optional 1 ms HID mode. The goal is to preserve a 1000 Hz mouse's report
rate through the Pi. Hardware measurements are still required; a successful
build or a 1000 Hz descriptor does not establish the actual forwarding rate.

## Build this fork on the Pi

Keep the working Pi gadget/OTG setup and cabling. Connect the mouse to a USB-A
host port and the Windows PC to the Pi's USB-C gadget port, using the same setup
that already works for USBProxy. Stop the existing USBProxy process first.

```sh
sudo apt update
sudo apt install -y git build-essential cmake pkg-config libusb-1.0-0-dev \
    libzmq3-dev cppzmq-dev libmsgpack-dev libboost-dev
git clone --branch fix/mouse-polling https://github.com/tyoueenn2/USBProxy.git
cd USBProxy
cmake -S src -B src/build -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_GREENGRASS=OFF -DBUILD_TESTING=ON
cmake --build src/build -j4
ctest --test-dir src/build --output-on-failure
sudo cmake --install src/build
sudo ldconfig
```

The AWS Greengrass executable is optional; it is not used for local USB
forwarding. This build uses the libraries available for the Pi's architecture.
The changes have been built and tested on x86-64; Pi/Windows hardware testing is
still needed. Old Nesto release binaries do not contain
these changes. If CMake 4 rejects the upstream minimum version, add
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` to the configure command.

## Run

Find the mouse's VID:PID with `lsusb`, then replace the example IDs below:

```sh
sudo /usr/local/bin/usb-mitm -v 046d -p c077 --mouse-1000hz --stats
```

`--mouse-1000hz` advertises a 1 ms interval on **all HID interrupt IN endpoints**
of the selected USB device, including any keyboard/vendor HID interfaces in a
composite mouse. It does not rewrite HID report formats, bulk endpoints or HID
OUT polling. The FS interval is 1 and the HS interval is 4 (eight 125 us
microframes). Without this option, the source's timing is preserved, with the
necessary conversion when the Pi's host-facing speed differs.

This option changes the host-facing descriptors only. It cannot make the
physical mouse produce reports faster than its firmware, selected profile, and
Linux host controller allow. Configure a 1000 Hz profile in the mouse itself
before proxying. A slower source can leave the host polling without new data.
Sub-millisecond/2000–8000 Hz operation is not promised by this 1 ms mode.

Replug the gadget cable or restart USBProxy after changing descriptor options so
Windows enumerates the new descriptors. Start without packet logging (`-l`,
`-w`, `-z`) or verbose debugging when measuring throughput. `-n` affects bulk
reads only and does not set the mouse rate.

## Measure where the rate changes

Move the mouse continuously and look for one line per second per active endpoint:

```text
[rate] mouse->Pi EP81: 999.2 reports/s
[rate] Pi->host EP81: 998.9 reports/s
```

These are illustrative values, not measured results. Input counts completed
nonempty interrupt transfers from the physical device; output counts completed
nonempty GadgetFS writes. Each endpoint has its own counter. Quiet endpoints
and the first interval after idle can show low values; compare steady movement.

- Input near 1000 and output near 1000: forwarding is keeping up. If a browser
  still shows 60, check a native Windows Raw Input polling-rate tool and, if
  needed, a USB trace. Browser pointer events can be coalesced or frame-limited.
- Input near 1000 and output near 60: the bottleneck is toward Windows. Inspect
  the gadget's interrupt endpoint with USB Device Tree Viewer/USBView. For a
  1 ms endpoint, expect `bInterval=4` at high speed or `1` at full speed. Run once
  with `-d` to print the endpoint intervals configured on the Pi, then disable
  debug logging for the measurement. Check cable, gadget driver and OS details.
- Input itself is slow: inspect the physical mouse's endpoint `bInterval` with
  `sudo lsusb -v -d VID:PID` and negotiated speed with `lsusb -t` on the Pi.
  Compare the mouse's onboard profile and the native Windows driver settings.

The async input queue retains reports in order and keeps eight USB reads
pending. If the host falls behind by 256 queued reports, USBProxy reports an
overflow and resets the device session rather than silently dropping movement
or button transitions. Include the rate lines, device descriptors, Pi OS/kernel
version and mouse model when reporting a remaining slowdown.

## What changed

The old code converted FS interrupt intervals only in the high-speed EP0
configuration blob, used floating-point logarithms, traversed only the first
alternate setting, and also tried to convert bulk endpoints. Endpoint setup
then reused the unconverted interval, and the other-speed interface lookup used
the alternate-setting index as the interface number. The conversion is now
shared by advertised configurations and configured endpoints, and native HS
intervals are kept intact. Integer conversion maps, for example, FS 1 ms to
HS `bInterval=4` and FS 16 ms to HS `bInterval=8` (62.5 Hz).

Interrupt IN uses a continuously rearmed eight-transfer libusb pipeline, with
ordered delivery, finite relay waits, and cancellation callbacks drained before
the device handle/context are closed. Bulk forwarding retains its existing
behavior. Tests cover descriptor conversion, composite/alternate interfaces,
malformed descriptor bounds, report order, rearming, zero-length packets,
timeouts, overflow, unplug and submission failures, and cancellation cleanup.
