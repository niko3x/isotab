# IsoTab

A small Linux browser-session manager written in C with GTK 3. Run several
browsers at once, each with its own cookies, logins, extensions and history.
There is no background service or additional language runtime.

## License

Copyright (C) 2026 niko3x. IsoTab is licensed under the
[GNU General Public License version 3](LICENSE) (`GPL-3.0-only`).
It is provided without warranty; see LICENSE for the full terms.

## Usage

Click **Add session**, choose an installed browser, and optionally name the
session. Launch it with one click afterward. The browser assignment is fixed:
create another session to use a different browser rather than sharing its data.

Supported browsers:

- Firefox and Firefox ESR
- LibreWolf
- Tor Browser 15 or newer (installed bundle)
- Chromium and Google Chrome
- Brave, Microsoft Edge and Vivaldi

Native browsers are detected in PATH. Installed Flatpak variants of Firefox,
LibreWolf, Tor Browser Launcher, Chromium, Chrome, Brave, Edge and Vivaldi are
separate choices. Snap variants of Firefox, Chromium and Brave are also separate
choices. A native browser and its packaged variant can run simultaneously.
Package discovery runs in the background; a newly installed Flatpak can take up
to 30 seconds to appear. Snap wrappers are excluded from native detection.

Flatpak launches grant access to the selected profile directory for that launch.
IsoTab does not create persistent Flatpak overrides. Chromium singleton markers
written inside a Flatpak namespace are translated to a host process using its
namespace PID and profile arguments/open files. Unverifiable owners remain
protected. Snap profiles use `~/snap/<browser>/common/isotab/profiles/<session ID>`
because strict Snap confinement cannot access `~/.isotab`. Session metadata stays
in `~/.isotab`; back up Snap profile folders separately.

Tor Browser uses the installed bundle's browser and Tor executables with a fresh
profile and separate `tor-data` directory for each session. It reads the bundle's
distributed Tor defaults without copying its personal profile or Tor state.
SOCKS ports are assigned automatically and control connections use Tor's unique
IPC directories. Stop also signals a verified session-specific Tor daemon through
a process descriptor; Clear/Remove/deletion remain blocked while Tor retains its
data lock. This is profile and Tor-state separation, not a guarantee of distinct
exit relays or unlinkability between sessions.

Run Tor Browser Launcher once to download and verify its bundle before using
IsoTab, including for its Flatpak variant. Native bundles installed by the launcher
or under `~/tor-browser/Browser` are detected. For another location choose the
installation folder in Settings. Tor's normal connection screen appears on first
launch. IsoTab manages a marked block in `user.js` for Tor data paths and the
control socket setting; custom lines after that block are preserved. Unmanaged
`user.js` files are refused rather than overwritten. Tor profiles cannot be
imported as ordinary Firefox profiles or vice versa.

Alt+1 through Alt+0 launch the first ten sessions. The scrollable list has no
fixed ten-session limit. Ctrl+Q closes the launcher and leaves browsers open.
Sessions opened by a previous launcher reconnect when their live profile lock
and browser process can be verified. They show Running with a restored Stop
button. Unverified or inaccessible profiles remain protected and must be closed
from the browser window. Reconnected Stop uses Linux process descriptors to
avoid signaling a reused PID; kernels without that facility require closing the
browser window.
If a browser is uninstalled, its sessions and data remain available for later.
Detection is refreshed while the launcher is open.

Each session card shows its name, browser, status and one Launch/Stop button.
The three-dot menu opens and closes with native GTK popover transitions
(respecting the desktop animation setting) and contains Rename, Clear profile
and Remove session. Add session uses neutral styling; Settings
contains preferences and Recovery.

Clear asks for confirmation, swaps in an empty profile and keeps the original
as a separate entry in **Settings > Recovery**. Remove moves the session to
Recovery without deleting its files. Restore returns it to the session list;
it never overwrites another session. Recovery also offers Import unlisted profile
for folders kept by older versions; choose their original browser. Rename changes the label without changing
its browser or profile location. Clear and Remove are blocked while in use.
Clear and Remove are recoverable and do **not** free disk space. Recovery shows
when each entry was saved and calculates its size in the background. Older
entries without timestamps show Date unavailable.

**Delete permanently** in Recovery asks for confirmation and frees disk space.
It cannot be undone. A locked browser blocks deletion. The owned profile is
moved into private `~/.isotab/trash/<session ID>` under its native lock before
removal (Snap uses `~/snap/<browser>/common/isotab/trash` on the same filesystem
as its profiles). Interrupted deletion stays marked as incomplete, cannot be restored,
and offers Retry delete. Missing folders offer **Forget entry**, which only
removes metadata. Restore rejects missing or inaccessible folders instead of
creating an empty replacement.

Import requires an explicit original browser choice. Recognized Firefox and
Chromium profiles reject incompatible adapters; folders containing both families
are rejected. Empty/unrecognized folders still require that explicit choice.

Profile separation does not provide separate network
identities or an additional operating-system sandbox.

## Settings

The Settings button opens Preferences, Storage, Recovery and Tips tabs. Choose the default
browser for new sessions, show or hide profile sizes, and enable or disable
Alt+number quick launch, and select a native Tor Browser installation folder.
Save applies these preferences and persists them;
Cancel leaves them unchanged. Ctrl+Q remains available.

Storage shows copyable paths for every session, the profile root and the settings
file, with an Open data folder action. Tips explain session management, shortcuts,
browser support and backups. Existing profiles are not moved by these settings.

## Existing data and storage

On first startup, IsoTab imports existing `~/.isotab/session_0` through
`session_9` directories as Firefox sessions, in place. Browser data is never
moved or rewritten by migration. Empty slots are replaced by Add session.

Session names and browser choices are stored in `~/.isotab/sessions.ini`.
Native and Flatpak profiles live under `~/.isotab/profiles/<session UUID>`;
Snap profiles use their browser's common data directory described above. Legacy profiles
keep their original paths. Settings are saved atomically with owner-only
permissions. Only one new launcher instance can edit settings at a time.
Close any older IsoTab launcher before running this version.

Firefox resets hold both profiles' `.parentlock` record locks. Stale modern
`IP:+PID` markers are accepted only after that lock is acquired. Unknown
legacy locks and inaccessible directories block management.

Chromium sessions check `SingletonLock` host and process information. A reset
reserves both profiles using Chromium's foreign-host lock behavior. A verified
dead local marker is replaced atomically, inspecting the displaced marker and
rechecking its owner before proceeding. Live, foreign and malformed markers
block management. Clear works after Stop without a relaunch. Abandoned IsoTab
reservations from an interrupted reset are recovered safely before launch.

Clear persists its Recovery entry before exchanging the original directory
with a newly created empty directory using Linux `renameat2(RENAME_EXCHANGE)`.
No original file is recursively deleted. An interruption leaves the original
in either the active or recovery path; a failed reset can leave an empty
recovery entry. Unsupported filesystems fail without deleting profile data.
Directory exchanges, durability checks and profile size scans run in background
workers. Profile actions and closing IsoTab wait for a reset to finish.

Symlinked profile roots, shared-writable directories and non-regular or
hard-linked lock files are rejected. Recovery is useful for accidental Clear
or Remove, but is not a separate backup against disk failure. Back up the
whole data folder with browsers closed.

Locking and profile arguments follow the upstream implementations:

- https://github.com/mozilla/gecko-dev/blob/master/toolkit/profile/nsProfileLock.cpp
- https://chromium.googlesource.com/chromium/src/+/main/docs/user_data_dir.md
- https://github.com/chromium/chromium/blob/main/chrome/browser/process_singleton_posix.cc

## Build and install

Requires a C compiler, Make, pkg-config, GTK 3.24+ development files, GLib 2.66+
(including glib-compile-resources), an SVG loader (librsvg), and `du` for profile
sizes. Install at least one supported browser separately. Flatpak and Snap
adapters use the package manager already installed on your system; IsoTab does
not install browsers, runtimes or Tor bundles.

Build with `make`, then run `./isotab`. The icon is embedded in the executable.
`bash install.sh` installs build dependencies and builds; it does not download
browsers or install the application system-wide.

Install with `make install` using appropriate permissions, or use
`make install PREFIX="$HOME/.local"` for a user installation. Ensure the chosen
prefix's `bin` directory is in your desktop session's PATH. `DATADIR` defaults
to `$(PREFIX)/share`; `DESTDIR` supports package staging. Use matching paths
when uninstalling. Install and uninstall do not change browser profiles.

## Checks

CI builds and tests on Ubuntu 22.04 and 24.04, plus a Debian 11 container with
actual GLib 2.66 headers and libraries. Compiler API limits reject accidental
use of newer GTK/GLib calls. Fortification selects level 3 when the compiler and
libc support it, otherwise level 2. The Debian 11 job is a compatibility check,
not a recommendation to deploy an older distribution.

`bash ci/kernel-vm.sh` boots a checksum-verified Ubuntu 22.04 cloud image in
QEMU, requires a real Linux 5.15 kernel, and runs the build, unit/security and
headless UI checks there. It needs QEMU, cloud-localds, curl and OpenSSH; its
source comes from the current Git commit. Container tests alone do not test an
older kernel. The VM is separate from the host and is deleted after testing.

Safe reconnect/Stop requires Linux 5.3+ process descriptors; Clear and permanent
deletion require filesystem support for the Linux renameat2 operations. Tests
also simulate missing/denied syscalls and verify that Stop refuses safely and
Clear/Delete preserve the original profile. Linux 5.15 is the tested kernel
baseline; older kernels are not claimed as fully supported.

Build distributable binaries on the oldest supported target environment,
rather than copying a binary built on a rolling distribution. CI coverage
does not make a locally compiled binary portable to older libc versions.

`make test` covers Firefox and Chromium lock handling, stale markers, symlink
safety, migration, settings round-trips, invalid settings, argument handling,
recoverable resets, interruption points, rollback, rename, restore, marker races,
import compatibility, guarded/retried permanent deletion, packaging arguments,
asynchronous Flatpak discovery, Snap storage, Tor profile path initialization,
Tor daemon locking, sandbox marker ownership and unavailable kernel operations.

`make integration-test` launches Firefox and two sessions of an installed
Chromium-family browser headlessly, using temporary profiles. It verifies
simultaneous operation, active-profile protection, reset and permanent deletion
of test recovery profiles after exit.
It skips when the required browsers are absent. Firefox and Google Chrome
have been exercised live; the other browser entries share their family adapter
but have not yet been tested individually.

`make ui-test` requires an available desktop display. It exercises background
Clear, removal, Settings Recovery, rename, importing an unlisted profile and
restart persistence using a temporary HOME. It also checks recovery sizes,
cancel/confirm deletion, forgetting missing entries and dialog safety during
background deletion.

`make package-integration-test` exercises two installed Tor Browser sessions,
including independent daemons, reconnect/Stop, active reset protection and
reset/purge after exit. It skips Tor when no supported bundle is available.
Set `ISOTAB_TEST_FLATPAK_APP` to an installed application ID to additionally run
an inert test shell inside its existing runtime. That check verifies real PID
namespace translation, the profile filesystem grant, active-profile protection
and stale-marker recovery; it does not launch the application's normal UI.
`make snap-integration-test` launches two profiles for each installed supported
Snap browser using fresh UUIDs in its common data directory. It checks concurrent
profiles, reconnect/Stop, active reset/purge refusal, recovery preservation and
cleanup without loading or saving the real session list.

Tor 15.0.22, a real Flatpak namespace, Firefox Snap 157.0-1 and Chromium Snap
154.0.8037.57 have been exercised here. The Snap run used Arch/EndeavourOS with
AppArmor disabled, so it does not establish full AppArmor confinement coverage.
Individual Flatpak browser builds, LibreWolf and Brave Snap still need
distribution testing.

Packaging references:

- [Flatpak run and per-launch permissions](https://docs.flatpak.org/en/latest/flatpak-command-reference.html#flatpak-run)
- [Snap common data directories](https://snapcraft.io/docs/reference/administration/data-locations/)
- [Tor Browser Launcher installation and verification](https://github.com/torproject/torbrowser-launcher)

## Security boundaries

Run IsoTab as your regular desktop user. The app rejects root execution and
requires its data folder to be private (0700). Settings are bounded to 1 MiB
and loaded only from an owned regular file. See SECURITY-AUDIT.md for the
review scope, fixes, test evidence and remaining limitations.

## Downloadable packages

The [Releases page](https://github.com/niko3x/isotab/releases) provides x86-64
DEB (Ubuntu 22.04+, Debian 12+, Mint 21+), pacman (Arch, Manjaro, CachyOS),
RPM (RHEL 9+ family and Fedora), and AppImage (glibc 2.35+) downloads.
See each release's installation instructions and SHA256SUMS. ARM, RHEL 8 and
musl/Alpine are not supported by these binaries. The packages are release
assets, not entries in GitHub's container/package registry or distro repositories.

Native packages use system GTK3; the AppImage bundles GTK3 and restores the
host environment when launching browsers and external tools. It supports
`--appimage-extract-and-run` when FUSE is unavailable. Browsers are not bundled.
All formats use the existing profile locations; uninstalling a package does not
remove profiles. An older manual installation in `/usr/local/bin` can shadow a
package in `/usr/bin`; remove that old launcher after switching to a package.

The `Release packages` workflow builds DEB, pacman and AppImage on Ubuntu 22.04,
RPM on AlmaLinux 9, then installs and launches native packages on six clean
container environments before publishing a tagged prerelease. A manual workflow
run builds/test artifacts without publishing. Bundled library sources, application
source and a checksummed PKGBUILD accompany the release. `packaging/PKGBUILD`
is a template; use the filled-in recipe attached to the release.
