# Changelog

Every release stays up, on nullmothsystems.com/probe and on the GitHub releases page, with its SHA-256.

## 2.2.0 - 2026-10-13

The whole machine, from every side: the stick, Windows, and macOS.

- **New: scan from Windows.** In the app, click "Scan this PC from Windows (about 3 minutes)". It records the
  processor, graphics, displays, firmware tables, Wi-Fi, Bluetooth, network, sound, battery and every device with its
  IDs and driver, and puts one .zip on your Desktop to send. It only reads and never goes online.
- **New: scan a Mac.** `bash 1401-probe-mac.sh --scan` scans a Mac or Nullintosh (no password, about a minute).
- **Wi-Fi and Bluetooth chips in depth.** Each chip's firmware version and what it can do, and the Bluetooth
  controller's version and features. This step is now included by default: press N at the question to skip it.
- **Processors:** the chip's family name (Tiger Lake, Vermeer, Granite Ridge and so on), power and turbo limits,
  frequency and sleep-state tables, and the timer speed measured.
- **Intel integrated graphics:** what Linux's driver reports about the GPU and a snapshot of its display registers.
- **Older NVIDIA cards (Kepler: GTX 600/700, Quadro K, Tesla K):** NVIDIA's own 470 driver is back on the stick, patched
  to run on the new kernel, so the trace records NVIDIA's own start-up on these cards too, not only the open-source one.
- The Mac and Linux tools are attached to the GitHub release (the site's links to them were broken).
- Never saved, on any of the three: serial numbers, UUIDs, network and Bluetooth addresses, Wi-Fi network names,
  the names of paired devices, and the computer's and your user's names.

## 2.1.0 - 2026-10-11

Every graphics card and integrated GPU now counts, old and new, whatever the brand. This is how NullMoth learns the
GPUs macOS has never supported: Intel Iris Xe and Arc, Radeon RX 7000 and 9000, Ryzen AI, and older cards too.

- **New last step: a GPU test.** Linux's own graphics drivers (Mesa: Intel, AMD and NVIDIA) run a short compute test
  and an off-screen drawing test on each GPU and check every result. Nothing is shown on the screen. The report gets
  what each driver told the GPU to do, which is what a macOS driver for that GPU has to do the same way.
- **Every GPU is scanned and traced**, not only NVIDIA, AMD and Intel ones we already support.
- Same safety as before: your drives stay locked read-only, nothing leaves the computer, and if a test hangs the PC
  turns itself off and the next start skips that test.
- The stick is about 80 MB larger (the graphics drivers for the test ride on it).

## 2.0.2 - 2026-10-09

- **No more freezes in the last step on laptops.** Every driver test in the last step now runs under the same
  automatic power-off as the graphics test: if one hangs, the PC turns itself off and the next start skips it.
  The HDMI audio of an NVIDIA card that is not driving the screen is no longer tested; it hung RTX 4050 laptops.
- **A freeze while listing PCI devices no longer loses the graphics card.** Before, every later scan skipped all of
  PCI and the report had no GPU in it.

## 2.0.1 - 2026-10-09

Fixes the errors some of you saw on 2.0.0. If you made a 2.0.0 stick, make it again with 2.0.1.

- **Driver trace errors fixed.** On 2.0.0 the last step printed "Bad file descriptor" / "write to closed file"
  errors and stalled for about a minute per device. The trace reader now stops cleanly.
- **NVIDIA's own driver now actually loads in the trace.** 2.0.0 tried to unload nouveau while it was driving the
  screen, which fails, so the NVIDIA trace recorded nothing. The card is now handed over directly.
- Instructions everywhere (site, stick, Windows app) match what the scan does: the Wi-Fi/Bluetooth question at the
  start, about 5 to 20 minutes, a black screen of up to 10 minutes near the end.
- New: `1401-probe-linux.py` makes the stick from Linux and finds your report afterwards.

## 2.0.0 - 2026-10-08

The probe now maps the whole machine, down to how each driver talks to its hardware, so the installer can build the
right setup for your exact PC.

- **Full system map.** Which GPU drives each screen and the laptop panel, switchable-graphics hints, GPU features
  (architecture, CUDA level, RT and tensor cores), PCIe slots, memory modules (no serials), storage ports and modes
  (AHCI/RAID/VMD), fans and sensors, USB roles (Bluetooth, camera, RGB, fingerprint), touchpad and keyboard bus,
  CPU core layout, IOMMU and the firmware memory map.
- **Build report.** The stick also writes a `Report.json` in the format the EFI builder reads (collector from
  Hardware-Sniffer, BSD-3).
- **Driver trace.** At the end, each graphics and audio driver is restarted while the kernel records every register
  read and write it makes. NVIDIA cards get NVIDIA's own driver for their generation, carried on the stick:
  open 610 for Turing and newer, 580 for Maxwell/Pascal/Volta (Kepler and older: nouveau for now).
  The screen may go black for a few minutes during this step.
- **Wi-Fi, Bluetooth and Ethernet: your choice.** The stick still has no network drivers in it. At the start it asks
  (default: no). If you press Y, it loads only the drivers for your own chips, records them starting, switches each
  radio off the moment it appears, and removes the drivers again. Nothing is configured or sent.
- Traces are redacted like the rest of the report (including a network chip's address as it appears in its
  registers) and capped at 40 MB.

## 1.0.1 - 2026-09-26

Fixes from the first round of test reports. Thanks to everyone who ran 1.0.0.

- **A freeze no longer loses the rest of the scan.** Before it touches a piece of hardware, the probe now writes the
  name of that step to the stick. If the PC locks up, start the stick again and pick the normal scan: it skips the
  one step that froze and finishes everything else. The report says which step was skipped and why.
- **The PCI scan runs one device at a time.** In 1.0.0 one desktop froze inside the PCI stage, and its report kept
  only the three stages before it. Now every device's IDs and addresses are saved first, and each device's full
  decode runs on its own afterwards, so a freeze points at one device instead of taking the whole stage.
- **Laptop graphics BIOS.** On laptops with a second GPU, the firmware often leaves the card's memory decoding off,
  so 1.0.0 could not read the VBIOS or the chip ID. The probe now turns decoding on for that one read and puts the
  original setting back right after.
- **Graphics driver data.** Each GPU's debugfs folder is read once instead of three times. The report now lists what
  the driver actually exposed, and every file it could not read is recorded with the reason.
- **Reports are sealed.** Each report has a `seal.json`: the SHA-256 of every file, plus a keyed check over that
  list. It tells us whether a report came straight off the stick or was changed afterwards. Nothing is sent
  anywhere and nothing in the report changes. Builds from source use their own key or none (`"sealed": false`).
- The PCI stage gets 3 minutes instead of 2.

## 1.0.0 - 2026-09-25

First public release.
