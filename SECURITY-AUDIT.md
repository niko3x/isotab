# IsoTab security review

Initial review: 2026-10-02; follow-up: 2026-10-03. Scope: application source,
browser descriptors, build and installation scripts, tests, desktop entry,
SVG/resource definitions, and the resulting executable in this workspace.
The initial review predated the Git repository; the follow-up included the
tracked files and local changes, but not a remote dependency audit.

## Result

The review identified and fixed local filesystem race conditions and validation
weaknesses. These findings primarily concern data integrity and availability;
this review did not demonstrate remote code execution or privilege escalation.
There is no basis to describe the application as unconditionally secure.

## Fixed findings

### Medium: path-based deletion could follow a substituted directory

The previous code queried a path without following symlinks, then separately
opened/enumerated that path. A process able to modify the profile concurrently
could substitute a symlink between those operations. The initial no-follow
query did not protect the later lookup. Intermediate components of profile
paths also were not checked.

The production Clear action now avoids recursive deletion entirely. It pins
both owned profile directories, acquires their native browser locks, persists
a Recovery entry, and atomically exchanges the original directory with a fresh
one. Remove only archives metadata. Original browser files remain available
for Restore even if the launcher exits between metadata saving and the exchange.
Clear/Remove preserve files. Only the explicitly confirmed permanent-delete
action in Recovery uses descriptor-relative deletion, after detaching a natively
locked profile into private trash and persisting an irreversible-deletion status.

Evidence: `profile-reset.h`, `prepare_recovery`, `remove_session`, and
`tests/reset_test.c`. Tests cover both sides of an interrupted exchange,
rollback, metadata-save failure, retained contents and restored entries.

### Medium: Chromium stale-marker removal could erase a newly acquired lock

The reservation replaces a verified dead local marker with `renameat2` exchange,
then validates the full displaced marker's hostname, decimal PID range and
owner's liveness. Each independently read snapshot receives the same validation. A changed
marker forces rollback. Live, foreign and malformed markers remain blocked.
The two profile reservations stay held during the directory exchange. A dead
IsoTab reservation is recovered through the same mechanism before launching.

Live integration verifies Firefox and two Chrome sessions, rejection of resets
while running, safe reconnected Stop, and successful reset after exit without
manually removing stale Chromium locks. Original sentinel files remain in
Recovery. Chromium's foreign-host reservation behavior is tested against an
actual native browser.

### Low: special lock files could hang or bypass expected lock semantics

The Firefox lock was opened for writing without nonblocking mode or validating
its type. A FIFO could hang the GTK thread. Regular-file type, ownership,
link count and write permissions were not validated consistently for other locks.

All lock opens now use nonblocking, no-follow flags and accept only owned,
single-link regular files without group/other write permission. Tests cover
FIFOs, hard links and profile path aliases.

### Low: session settings accepted unsafe file types and unbounded input

Settings loading followed paths and did not bound file size. It now reads from
an owned, single-link regular file through a validated directory descriptor,
rejects symlinks/shared-writable files, and enforces a 1 MiB limit while reading
and writing. Saves remain atomic and anchored to the opened data directory.
Tests cover FIFO/symlink/oversized settings, invalid session IDs, rollback and
round-trip persistence.

### Defense in depth: privilege and build assumptions

IsoTab now refuses root execution and requires its data directory to be owned
by the desktop user and mode 0700. Profile directories must be owned and not
shared-writable. These checks fail with an explanation instead of changing
existing user permissions.

The build now explicitly enables stack protection, fortified libc checks, PIE,
full RELRO and a non-executable stack. ELF inspection confirms PIE, GNU_RELRO,
BIND_NOW and a read/write-only GNU_STACK. The installer anchors itself to its
own directory and uses strict shell error handling.

## Verification

- `make test`: Firefox safety, session persistence/migration, Chromium locking,
  removal behavior, preferences, invalid settings and new security cases passed.
- The race test performs 1,000 deletion attempts against a directory/symlink
  exchange loop and verifies a sentinel outside the profile survives.
- `make integration-test`: actual Firefox plus two Google Chrome profiles ran
  together in temporary directories. Active-profile clearing was blocked.
  Stale Chromium markers were refused. Marker-free profiles were cleared, and
  Chrome respected the deletion reservation with desktop dialogs unavailable.
- AddressSanitizer and UndefinedBehaviorSanitizer passed all three unit-test
  programs. Leak detection was disabled; this is not a leak-clean claim.
  The first sanitizer attempt could not start due to the environment's preload
  order; rerunning with the sanitizer runtime first succeeded.
- GCC `-fanalyzer` compilation completed without diagnostics.
- Bash syntax validation passed. No matches were found for the limited private
  key, AWS access-key and GitHub-token patterns scanned in workspace files.
- Settings UI checks exercised Save, Cancel and persistence using temporary HOME.
- SVG/resource inspection found no scripts, external references or remote assets.

Tool/library versions observed: GCC 16.2.1, GTK 3.24.52, GLib 2.88.3,
librsvg 2.62.4. This is an inventory, not a dependency-CVE clearance.

## Remaining limitations

- Browser profiles are data separation, not a sandbox against other programs
  running as the same Unix user. A same-user attacker can change data directly.
- Chromium's reservation uses its native foreign-host lock behavior. An explicit
  override in a separately launched browser, or removal of the lock by another
  process, is outside the protection this launcher can guarantee. Do not override
  a browser lock warning while Clear is running.
- Directory symlinks in the data path are intentionally rejected. Same-device
  bind mounts are not fully distinguished from directories; this review does
  not claim protection against privileged filesystem/mount manipulation.
- Clear and Remove retain disk usage. Recovery has a separate confirmed permanent
  deletion action that reclaims space. It is not secure erasure of SSD snapshots,
  filesystem backups or remapped blocks. An interrupted purge can partially
  remove files; its entry stays non-restorable and offers retry/cleanup.
- Heavy profile work runs in background workers. Small metadata saves and
  status checks still run on the UI thread, so this does not guarantee
  responsiveness on a stalled filesystem.
- Directory exchange requires Linux/filesystem support for renameat2 exchange.
  A failure is reported without recursive deletion of the original profile.
- Native browser executables and `du` are resolved through the user's PATH;
  the desktop environment and executable search path must be trusted.
- Firefox and Chrome received live integration tests. Other declared browser
  variants, sandboxed packaging, other desktops, network filesystems and
  dependency CVE status were not comprehensively tested in this review.

## Follow-up: reconnecting browser sessions

The launcher now discovers an earlier browser using the live Firefox record-lock
owner or Chromium singleton marker. It verifies the Unix owner, browser
executable family, and profile arguments (or Chromium's open profile files when
headless mode has flattened its process title). It never trusts a saved PID.
Reconnected Stop opens a Linux PID descriptor, rechecks the profile owner and
process identity, and signals through that descriptor so PID reuse cannot
redirect the signal. Clear and Remove remain blocked while the profile is locked.
A forged marker pointing to a non-browser is rejected by the security tests.
Live integration exercises a separate observer process reconnecting and stopping
Firefox and two Chrome sessions using temporary profiles.

Linux interface references:
- https://man7.org/linux/man-pages/man2/pidfd_open.2.html
- https://man7.org/linux/man-pages/man2/pidfd_send_signal.2.html

## Follow-up: recovery validation and cleanup

Restore rejects absent or inaccessible profile folders before changing metadata.
Import requires explicit browser selection and rejects recognizable incompatible
or mixed-family data. Recovery dates are persisted and sizes are asynchronous.
Legacy entries retain compatibility, with unavailable dates shown explicitly.

Permanent deletion saves deletion_started before worker execution. The worker
holds native browser locks, validates directory identities, atomically detaches
the profile into private trash, and deletes without following symlink contents.
Failures before detachment roll back the deletion flag when settings can be saved.
Failures after detachment retain an incomplete, non-restorable entry; retry resumes
from trash. Missing entries can be forgotten only after both possible locations
are confirmed absent. Successful removal of metadata follows successful deletion.

Evidence: tests/lock_race_test.c injects foreign, malformed and live replacements
between validation reads and exchange. tests/recovery_test.c covers absent
restores, incompatible/mixed imports, dates, native lock refusal, outside-symlink
protection and depth-limit interruption/retry. tests/ui_recovery_test.c covers
sizes, cancellation, confirmed deletion, pending-dialog protection and forgetting.

## Follow-up: profile directory creation

Launch, reset and permanent deletion previously called `g_mkdir_with_parents`
before validating profile storage paths. A substituted `profiles` or `trash`
symlink could make that call create a directory outside IsoTab's data folder,
even though the following access check rejected the path. These operations now
create a single child through an already validated parent directory descriptor
and reject symlinks. A regression test confirms that an outside directory stays
untouched. Build, unit, UI and live Firefox/Chrome integration checks passed
after the change; the tests used temporary profiles.
