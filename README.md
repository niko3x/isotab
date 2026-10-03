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

Supported native executables detected in PATH:

- Firefox and Firefox ESR
- Chromium and Google Chrome
- Brave, Microsoft Edge and Vivaldi

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
removal. Interrupted deletion stays marked as incomplete, cannot be restored,
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
Alt+number quick launch. Save applies these preferences and persists them;
Cancel leaves them unchanged. Ctrl+Q remains available.

Storage shows copyable paths for every session, the profile root and the settings
file, with an Open data folder action. Tips explain session management, shortcuts,
browser support and backups. Existing profiles are not moved by these settings.

## Existing data and storage

On first startup, IsoTab imports existing `~/.isotab/session_0` through
`session_9` directories as Firefox sessions, in place. Browser data is never
moved or rewritten by migration. Empty slots are replaced by Add session.

Session names and browser choices are stored in `~/.isotab/sessions.ini`.
New profiles live under `~/.isotab/profiles/<session UUID>`. Legacy profiles
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

Requires a C compiler, Make, pkg-config, GTK 3 development files, GLib 2.66+
(including glib-compile-resources), an SVG loader (librsvg), and `du` for profile
sizes. Install at least one supported native browser separately. Flatpak and
Snap launch adapters are not implemented.

Build with `make`, then run `./isotab`. The icon is embedded in the executable.
`bash install.sh` installs build dependencies and builds; it does not download
browsers or install the application system-wide.

Install with `make install` using appropriate permissions, or use
`make install PREFIX="$HOME/.local"` for a user installation. Ensure the chosen
prefix's `bin` directory is in your desktop session's PATH. `DATADIR` defaults
to `$(PREFIX)/share`; `DESTDIR` supports package staging. Use matching paths
when uninstalling. Install and uninstall do not change browser profiles.

## Checks

`make test` covers Firefox and Chromium lock handling, stale markers, symlink
safety, migration, settings round-trips, invalid settings, argument handling,
recoverable resets, interruption points, rollback, rename, restore, marker races,
import compatibility and guarded/retried permanent deletion.

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

## Security boundaries

Run IsoTab as your regular desktop user. The app rejects root execution and
requires its data folder to be private (0700). Settings are bounded to 1 MiB
and loaded only from an owned regular file. See SECURITY-AUDIT.md for the
review scope, fixes, test evidence and remaining limitations.
