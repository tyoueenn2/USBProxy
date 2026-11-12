# Native mouse polling rates on Raspberry Pi 4

USBProxy preserves the mouse's advertised interrupt polling interval and forwards
each incoming report as it arrives. There is no polling-rate setting in the
proxy. A mouse producing 500, 1000 or 8000 reports/s uses the same forwarding
path, without a timer that resamples, duplicates or discards reports to reach a
chosen rate.

High-speed USB interrupt intervals are kept unchanged. When a full-speed source
is represented to Windows at high speed, only the descriptor's interval encoding
changes, preserving the elapsed polling interval where representable. For
intervals that are not representable exactly, the host-facing schedule is rounded
toward faster polling so the source's reports need not be throttled.

## Build this fork on the Pi

Keep the working Pi gadget/OTG setup and cabling. Connect the mouse to a USB-A
host port and the Windows PC to the Pi's USB-C gadget port, using the same setup
that already works for USBProxy. Stop the existing USBProxy process first.

```sh
sudo apt update
sudo apt install -y git build-essential cmake pkg-config python3 libusb-1.0-0-dev \
    libzmq3-dev cppzmq-dev libmsgpack-dev libboost-dev
git clone https://github.com/tyoueenn2/USBProxy.git
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
sudo /usr/local/bin/usb-mitm -v 046d -p c077 --stats
```

`--stats` enables measurement only. It does not change polling or report contents.
The mouse's firmware, active profile and USB endpoint descriptors determine its
report cadence. USB control requests for the mouse are still forwarded.

| Mouse/host-facing USB schedule | bInterval | Polling interval | Maximum scheduled polls/s |
| --- | --- | --- | --- |
| Full speed | 2 | 2 ms | 500 |
| Full speed | 1 | 1 ms | 1000 |
| High speed | 5 | 2 ms | 500 |
| High speed | 4 | 1 ms | 1000 |
| High speed | 3 | 500 us | 2000 |
| High speed | 2 | 250 us | 4000 |
| High speed | 1 | 125 us | 8000 |

A polling interval defines bus service opportunities; an idle mouse need not
produce a report for every poll. An 8000 Hz schedule requires high-speed USB.
If the gadget negotiates full speed, its 1 ms minimum polling interval cannot
carry 8000 separate interrupt reports/s. Check negotiated speed rather than
changing rate settings in the proxy.

Replug the gadget cable or restart USBProxy after installing this version so
Windows enumerates the corrected descriptors. Start without packet logging
(`-l`, `-w`, `-z`) or verbose debugging when measuring throughput. `-n` affects
bulk reads only and does not set the mouse rate.

## Measure where the rate changes

Move the mouse continuously and look for one line per second per active endpoint:

```text
[rate] mouse->Pi EP81: 999.2 reports/s
[rate] Pi->host EP81: 998.9 reports/s
```

These are illustrative values for a 1000 Hz source, not measured results. An
8000 Hz source should instead be compared against approximately 8000 reports/s
on both sides; a 500 Hz source against approximately 500 reports/s. Input counts
completed nonempty interrupt transfers from the physical device; output counts
completed nonempty GadgetFS writes. Quiet endpoints and the first interval after
idle can show low values; compare steady movement on the same endpoint.

- Matching input/output rates near the source's configured rate indicate that
  forwarding is keeping up. If a browser still shows 60, check a native Windows
  Raw Input polling-rate tool and, if needed, a USB trace. Browser pointer events
  can be coalesced or frame-limited.
- Fast input with slow output locates the bottleneck toward Windows. Inspect the
  gadget's interrupt endpoint with USB Device Tree Viewer/USBView. Compare its
  negotiated speed and bInterval with the table above. Run once with `-d` to
  print the endpoint intervals configured on the Pi, then disable debugging for
  measurements. Check cable, gadget driver and OS details.
- Slow input locates the bottleneck before forwarding to Windows. Inspect the
  physical mouse's endpoint `bInterval` with `sudo lsusb -v -d VID:PID` and
  negotiated speed with `lsusb -t` on the Pi. Compare the mouse's onboard profile
  and the native Windows driver settings.

The async input queue retains reports in order and keeps eight USB reads pending.
If the host falls behind by 256 queued reports, USBProxy reports an overflow and
resets the device session rather than silently dropping movement or button
transitions. This is a backlog limit, not a report-rate limit. Include the rate
lines, device descriptors, Pi OS/kernel version and mouse model when reporting a
remaining slowdown.

## Validation and implementation

The changes have been built and tested on x86-64. Descriptor tests verify that
native high-speed schedules for 500, 1000, 2000, 4000 and 8000 Hz remain unchanged,
and that full-speed to high-speed conversion preserves 1, 2, 4, 8 and 16 ms
intervals. Both the advertised configurations and configured endpoints use the
same conversion. Fake-transport tests cover report order/rearming, idle timeouts,
ZLPs, overflow, unplug, submission errors and cancellation cleanup.

Preserving descriptors and passing software tests do not establish achieved
hardware throughput. Pi/Windows measurements are still required at each rate.
The interrupt input path continuously rearms transfers; the output path waits
for transfer completion instead of using a fixed reporting timer. Bulk forwarding
retains its existing behavior.

See [testing.md](testing.md) for the automated suites, sanitizer builds, CI and
a repeatable Pi/Windows hardware validation procedure.
