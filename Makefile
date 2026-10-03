# IsoTab - Browser Session Launcher
# C + GTK3, using browsers already installed on the system

CC      = gcc
TARGET  = isotab

CFLAGS  = $(shell pkg-config --cflags gtk+-3.0) \
          -O2 -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fPIE -Wall -Wextra -Wno-unused-parameter
LDFLAGS = $(shell pkg-config --libs   gtk+-3.0) -pie -Wl,-z,relro,-z,now,-z,noexecstack

PREFIX  ?= /usr/local
DATADIR ?= $(PREFIX)/share

all: $(TARGET)

test: isotab-resources.c
	@set -e; test_bin=$$(mktemp /tmp/isotab-test.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	for test_source in tests/profile_test.c tests/sessions_test.c tests/security_test.c tests/reset_test.c tests/lock_race_test.c tests/recovery_test.c tests/adapters_test.c; do \
	$(CC) $(CFLAGS) -Werror -o "$$test_bin" "$$test_source" isotab-resources.c $(LDFLAGS); \
	"$$test_bin"; done

isotab-resources.c: isotab.gresource.xml isotab.svg
	glib-compile-resources --generate-source --c-name isotab --target=$@ isotab.gresource.xml

$(TARGET): browser.c browsers.h safe-files.h tor-profile.h profile-reset.h profile-import.h profile-purge.h sandbox-process.h isotab-resources.c
	@echo "  CC  browser.c"
	@$(CC) $(CFLAGS) -o $(TARGET) browser.c isotab-resources.c $(LDFLAGS)
	@echo "  > ./$(TARGET) built OK"

install: $(TARGET)
	install -Dm755 $(TARGET)        $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -Dm644 isotab.desktop   $(DESTDIR)$(DATADIR)/applications/isotab.desktop
	install -Dm644 isotab.svg $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/isotab.svg
	gtk-update-icon-cache -f -t $(DESTDIR)$(DATADIR)/icons/hicolor
	@echo "  OK: Installed to $(PREFIX)/bin/$(TARGET)"
	@echo "  OK: Desktop entry installed - search 'IsoTab' in your app launcher"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -f $(DESTDIR)$(DATADIR)/applications/isotab.desktop
	rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/isotab.svg
	gtk-update-icon-cache -f -t $(DESTDIR)$(DATADIR)/icons/hicolor
	@echo "  OK: Uninstalled"

clean:
	rm -f $(TARGET) isotab-resources.c

.PHONY: all test clean install uninstall

integration-test: isotab-resources.c
	@set -e; test_bin=$$(mktemp /tmp/isotab-integration.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) $(CFLAGS) -Werror -o "$$test_bin" tests/integration_test.c isotab-resources.c $(LDFLAGS); \
	"$$test_bin"

.PHONY: integration-test

# Requires an available desktop display; fixtures use a temporary HOME.
ui-test: isotab-resources.c
	@set -e; test_bin=$$(mktemp /tmp/isotab-ui.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	for test_source in tests/ui_test.c tests/ui_recovery_test.c; do \
	$(CC) $(CFLAGS) -Werror -o "$$test_bin" "$$test_source" isotab-resources.c $(LDFLAGS); \
	G_DEBUG=fatal-warnings "$$test_bin"; done

.PHONY: ui-test

package-integration-test: isotab-resources.c
	@set -e; test_bin=$$(mktemp /tmp/isotab-package-integration.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) $(CFLAGS) -Werror -o "$$test_bin" tests/package_integration_test.c isotab-resources.c $(LDFLAGS); \
	"$$test_bin"

.PHONY: package-integration-test
