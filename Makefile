.PHONY: userland

OUT_DIR ?= ../testdir/binaries
BUILD_SCRIPT := ./devtools/build_user_c.sh
PACKAGES_DIR := ./packages

userland:
	@mkdir -p "$(OUT_DIR)"
	@for src in $$(find "$(PACKAGES_DIR)" -mindepth 2 -maxdepth 2 -type f -name '*_uelf.c' | sort); do \
		name=$$(basename "$$src" _uelf.c); \
		out="$(OUT_DIR)/$$name"; \
		echo "Building $$name ..."; \
		bash "$(BUILD_SCRIPT)" "$$src" "$$out" || true; \
	done