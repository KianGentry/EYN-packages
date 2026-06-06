.PHONY: userland

OUT_DIR ?= ../testdir/binaries
BUILD_SCRIPT := ./devtools/build_package.sh
PACKAGES_DIR := ./packages
CONFIG_GUI_ENABLED ?= 1
TTY_EXCLUDES ?=

userland:
	@mkdir -p "$(OUT_DIR)"
	@for pkg_dir in $$(find "$(PACKAGES_DIR)" -mindepth 1 -maxdepth 1 -type d | sort); do \
		name=$$(basename "$$pkg_dir"); \
		out="$(OUT_DIR)/$$name"; \
		if [ "$(CONFIG_GUI_ENABLED)" = "0" ]; then \
			skip=0; \
			for exclude in $(TTY_EXCLUDES); do \
				if [ "$$name" = "$$exclude" ]; then \
					skip=1; \
					break; \
				fi; \
			done; \
			if [ $$skip -eq 1 ]; then \
				echo "Skipping $$name (TTY mode)"; \
				continue; \
			fi; \
		fi; \
		echo "Building $$name ..."; \
		bash "$(BUILD_SCRIPT)" "$$pkg_dir" "$$out" || true; \
	done