# IsoTab - Browser Session Launcher
# C + GTK3, using browsers already installed on the system

CC      = gcc
TARGET  = isotab
VERSION = $(shell cat VERSION)

# Do not silently introduce APIs newer than our documented library baseline.
API_FLAGS = -DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_66 \
            -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_66 \
            -DGTK_VERSION_MIN_REQUIRED=GTK_VERSION_3_24 \
            -DGTK_VERSION_MAX_ALLOWED=GTK_VERSION_3_24 \
            -Werror=deprecated-declarations
# Level 3 needs both newer libc headers and dynamic object-size support.
FORTIFY_LEVEL = $(shell $(CC) -E -P -x c ci/fortify-level.c)

CFLAGS  = $(shell pkg-config --cflags gtk+-3.0) \
          $(API_FLAGS) -DISOTAB_VERSION=\"$(VERSION)\" -O2 -D_FORTIFY_SOURCE=$(FORTIFY_LEVEL) -fstack-protector-strong -fPIE -Wall -Wextra -Wno-unused-parameter
LDFLAGS = $(shell pkg-config --libs   gtk+-3.0) -pie -Wl,-z,relro,-z,now,-z,noexecstack

PREFIX  ?= /usr/local
DATADIR ?= $(PREFIX)/share

all: $(TARGET)

test: isotab-resources.c
	@set -e; test_bin=$$(mktemp /tmp/isotab-test.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	for test_source in tests/profile_test.c tests/sessions_test.c tests/security_test.c tests/reset_test.c tests/lock_race_test.c tests/recovery_test.c tests/adapters_test.c tests/kernel_test.c; do \
	$(CC) $(CFLAGS) -Werror -o "$$test_bin" "$$test_source" isotab-resources.c $(LDFLAGS); \
	"$$test_bin"; done

isotab-resources.c: isotab.gresource.xml isotab.svg
	glib-compile-resources --generate-source --c-name isotab --target=$@ isotab.gresource.xml

$(TARGET): browser.c host-environment.h VERSION browsers.h safe-files.h tor-profile.h profile-reset.h profile-import.h profile-purge.h sandbox-process.h isotab-resources.c Makefile ci/fortify-level.c
	@echo "  CC  browser.c"
	@$(CC) $(CFLAGS) -o $(TARGET) browser.c isotab-resources.c $(LDFLAGS)
	@echo "  > ./$(TARGET) built OK"

install: $(TARGET)
	install -Dm755 $(TARGET)        $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -Dm644 isotab.desktop   $(DESTDIR)$(DATADIR)/applications/isotab.desktop
	install -Dm644 isotab.svg $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/isotab.svg
	@if [ -z "$(DESTDIR)" ]; then gtk-update-icon-cache -f -t $(DATADIR)/icons/hicolor; fi
	install -Dm644 LICENSE $(DESTDIR)$(DATADIR)/licenses/isotab/LICENSE
	@echo "  OK: Installed to $(PREFIX)/bin/$(TARGET)"
	@echo "  OK: Desktop entry installed - search 'IsoTab' in your app launcher"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -f $(DESTDIR)$(DATADIR)/applications/isotab.desktop
	rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/isotab.svg
	@if [ -z "$(DESTDIR)" ]; then gtk-update-icon-cache -f -t $(DATADIR)/icons/hicolor; fi
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

snap-integration-test: isotab-resources.c
	@set -e; test_bin=$$(mktemp /tmp/isotab-snap-integration.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) $(CFLAGS) -Werror -o "$$test_bin" tests/snap_integration_test.c isotab-resources.c $(LDFLAGS); \
	"$$test_bin"

.PHONY: snap-integration-test
