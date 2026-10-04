Native packages and a portable AppImage are now available for x86-64 Linux.

Choose one download:

- **.deb**: Ubuntu 22.04+, Debian 12+, Linux Mint 21+. Install with `sudo apt install ./isotab_0.1.1_amd64.deb`.
- **.pkg.tar.zst**: Arch, Manjaro and CachyOS. Install with `sudo pacman -U ./isotab-0.1.1-1-x86_64.pkg.tar.zst`. A PKGBUILD with a verified source checksum is also supplied for rebuilding.
- **.rpm**: RHEL 9+ and compatible Enterprise Linux systems, Fedora. Install with `sudo dnf install ./isotab-0.1.1-1.el9.x86_64.rpm`. RHEL 8 is not supported.
- **.AppImage**: glibc-based Linux with glibc 2.35+. Make executable and launch: `chmod +x IsoTab-0.1.1-x86_64.AppImage`. Requires a usable FUSE device, or run with `--appimage-extract-and-run` without FUSE. Browsers remain installed separately. AppImage bundles GTK3 and its supporting libraries; source packages for bundled libraries are provided separately.

SHA256SUMS lists checksums for all downloads. These packages are not registered in distro repositories or AUR. The AppImage does not automatically update. ARM and musl/Alpine builds are not included.

Packages install the application, desktop entry, icon and license only. They do not migrate, remove or reset browser profiles. Existing profiles remain in `~/.isotab`; Snap browser profiles remain under `~/snap/<browser>/common/isotab`. Close IsoTab before updating. If you previously used `make install`, remove its older `/usr/local/bin/isotab` launcher so it does not shadow `/usr/bin/isotab` (do not remove your profile folders).

This update fixes compatibility with GLib 2.66 and tests older libraries and Linux 5.15. Package CI additionally installs and launches the downloadable binaries on Ubuntu 22.04/24.04, Debian 12, Arch, AlmaLinux 9 and Fedora. Mint, Manjaro, CachyOS and RHEL compatibility follows their respective package/library baselines; these derivatives are not separately tested desktop environments.

This remains a public beta. Some browser adapters still lack real-browser verification. Separate profiles are not an OS security sandbox; Tor profile separation does not guarantee unlinkability. Source is licensed GPL-3.0-only, with third-party bundled libraries under their respective licenses.
