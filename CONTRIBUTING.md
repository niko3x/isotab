# Contributing to IsoTab

Open an issue for a bug or proposed change. Include your Linux distribution,
desktop environment, GTK version, browser and steps to reproduce. Remove any
personal profile contents, credentials and browsing data from attachments.

Keep changes focused and preserve the native C/GTK implementation. Browser
profile safety and compatibility with existing data take priority over features.

Before submitting a pull request, run:

```sh
make
make test
make ui-test
bash -n install.sh
```

UI tests require a desktop display; `xvfb-run -a make ui-test` is an alternative.
For changes to browser launching or native locks, also run
`make integration-test` with Firefox and a Chromium-family browser installed.
Tests use temporary profile fixtures. Never test destructive profile operations
against your real browser data.

Report security vulnerabilities privately as described in SECURITY.md.
