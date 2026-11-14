# Linux port

The original Windows source, resources, and build files remain in this folder. The Linux entry point is `rufus-linux`. Its window follows the control layout, labels, and resource IDs in `src/rufus.rc` and `src/resource.h`.

## Run

Install the GTK 3 and JSON-GLib development packages, plus the Linux disk utilities:

```sh
sudo apt install build-essential libgtk-3-dev libjson-glib-dev util-linux udisks2 policykit-1
```

Then run `./rufus-linux` from this folder. It builds the Linux window on first launch and opens it. GTK uses your Linux desktop theme, while the control order and layout follow Rufus's original dialog.

The bottom toolbar's **Settings** button controls whether fixed USB drives appear in the device list, whether Rufus verifies the written image, and whether it safely powers off the USB drive after writing. These preferences are saved in `~/.config/rufus-linux/settings.ini`.

## Linux support in this port

The Linux build currently writes an ISO or raw disk image in DD mode. It lists removable drives, protects the drive containing `/`, unmounts mounted partitions, requires a confirmation before writing, and flushes the completed write. Optional verification compares the written bytes with the source image; optional power-off uses the desktop disk service. It does not yet implement Rufus's formatting, Windows installation customization, persistence, bad-block checking, or other Windows-specific backends. Those controls stay in their original places and are disabled when their Linux backend is not available.

The Windows application continues to use the original Visual Studio/MinGW build paths. This Linux front end is covered by the project's GPL-3.0-or-later license in `LICENSE.txt`.
