.DEFAULT_GOAL := build
CC := clang
LD := ld.lld
OBJCOPY := llvm-objcopy
NASM := nasm
OUT := build/skeleton
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only

.PHONY: image kernel container-image build mvp run runtime runtime-smoke \
        test test-files test-lisp test-tooling check-web test-probes test-guest \
        check-mvp lint check web web-assets web-serve test-browser help
.PHONY: linux-build linux-run linux-check

# A separate exploration; the default image and MVP acceptance stay unchanged.
linux-build:
	python3 experiments/linux/linux.py build

linux-run: linux-build
	python3 experiments/linux/linux.py run

linux-check: linux-build
	python3 experiments/linux/linux.py check
.NOTPARALLEL: check
BUILD_IMAGE := emacsos-skeleton:alpine3.22
KERNEL_C := main hardware memory loader runtime files file_abi
KERNEL_ASM := entry interrupts syscall
KERNEL_OBJS := $(addprefix $(OUT)/,$(addsuffix .o,$(KERNEL_C) $(KERNEL_ASM)))
image: $(OUT)/emacsos.img
kernel: $(OUT)/kernel.bin

$(OUT):
	mkdir -p $@

$(OUT)/%.o: kernel/%.asm Makefile scripts/Dockerfile.build | $(OUT)
	$(NASM) -f elf64 $< -o $@

$(OUT)/%.o: kernel/%.c kernel/kernel.h kernel/files.h Makefile scripts/Dockerfile.build | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/kernel.elf: $(KERNEL_OBJS) kernel/kernel.ld
	$(LD) -T kernel/kernel.ld -o $@ $(KERNEL_OBJS)

$(OUT)/kernel.bin: $(OUT)/kernel.elf
	$(OBJCOPY) -O binary $< $@

$(OUT)/boot.bin: kernel/boot.asm $(OUT)/kernel.bin Makefile
	@set -eu; size=$$(wc -c < $(OUT)/kernel.bin); \
	sectors=$$(((size + 511) / 512)); \
	test "$$sectors" -ge 1; test "$$sectors" -le 896; \
	$(NASM) -f bin -D KERNEL_SECTORS=$$sectors $< -o $@; \
	test "$$(wc -c < $@)" -eq 512

$(OUT)/emacsos.img: $(OUT)/boot.bin $(OUT)/kernel.bin
	@set -eu; size=$$(wc -c < $(OUT)/kernel.bin); \
	padding=$$(((512 - size % 512) % 512)); \
	cat $(OUT)/boot.bin $(OUT)/kernel.bin > $@.part; \
	dd if=/dev/zero bs=1 count=$$padding >> $@.part 2>/dev/null; \
	mv $@.part $@

build/toolchains/kernel.id: scripts/Dockerfile.build
	mkdir -p $(@D)
	docker build --platform linux/amd64 -t $(BUILD_IMAGE) -f $< scripts
	docker image inspect $(BUILD_IMAGE) --format '{{.Id}}' > $@

container-image: build/toolchains/kernel.id
	@if ! docker image inspect $(BUILD_IMAGE) >/dev/null 2>&1; then \
		docker build --platform linux/amd64 -t $(BUILD_IMAGE) -f scripts/Dockerfile.build scripts; \
	fi
	@if ! $(MAKE) --no-print-directory -q image; then \
		docker run --rm --platform linux/amd64 --user "$$(id -u):$$(id -g)" \
		-e HOME=/tmp -v "$(CURDIR):/work:z" -w /work $(BUILD_IMAGE) make image; \
	fi

run: build
	python3 scripts/qemu.py

runtime:
	./scripts/build-runtime.sh

runtime-smoke: runtime
	python3 tests/runtime/smoke.py

build: runtime container-image
	python3 scripts/image.py build

# Keep existing contributor and CI entry points as aliases.
mvp: build

test: test-files test-lisp test-tooling check-web

test-files:
	python3 tests/kernel/check-files.py

test-lisp:
	mkdir -p build/lisp
	emacs -Q --batch -L lisp \
	  --eval '(setq byte-compile-error-on-warn t byte-compile-dest-file-function (lambda (file) (expand-file-name (file-name-nondirectory (concat file "c")) "build/lisp/")))' \
	  -f batch-byte-compile lisp/emacsos-app.el lisp/emacsos-counter.el
	emacs -Q --batch -L lisp -l tests/lisp/emacsos-app-tests.el -f ert-run-tests-batch-and-exit

test-tooling:
	python3 -m unittest discover -s tests/tooling -p 'test_*.py'

check-web:
	python3 -m unittest discover -s tests/web -p 'test_*.py'

lint:
	python3 scripts/check-contribution.py --require-clean-submodule

test-probes: container-image
	python3 tests/kernel/check-probe.py

test-guest: build
	python3 tests/guest/check-mvp.py

check-mvp: test-guest

# Run in order even under make -j: probes and guest acceptance share kernel files.
check: test lint test-probes test-guest

WEB_HOST ?= 127.0.0.1
WEB_PORT ?= 18082
WEB_URL ?= http://$(WEB_HOST):$(WEB_PORT)/
web-assets:
	python3 scripts/fetch-web-assets.py

web: build web-assets
	python3 scripts/serve-web.py --host $(WEB_HOST) --port $(WEB_PORT)

web-serve: web-assets
	python3 scripts/serve-web.py --host $(WEB_HOST) --port $(WEB_PORT)

test-browser:
	node tests/web/browser.cjs '$(WEB_URL)'

help:
	@echo 'make linux-run - build and boot the separate Linux exploration'
	@echo 'make linux-check - accept the Linux exploration across two guest boots'
	@echo 'make build  - incrementally build the complete image (default)'
	@echo 'make run    - build and launch the native console'
	@echo 'make web    - build, verify browser assets and serve the browser console'
	@echo 'make test   - run host file, Lisp, tooling and web server tests'
	@echo 'make check  - run host tests, conventions, kernel probes and guest acceptance'
	@echo 'make test-browser - test an already running web server (optional Playwright)'
