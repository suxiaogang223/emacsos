.DEFAULT_GOAL := build
CC := clang
LD := ld.lld
OBJCOPY := llvm-objcopy
NASM := nasm
OUT := build/kernel
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
          -I kernel/lwip-libc -I kernel -I third_party/littlefs -I third_party/lwip/src/include \
          -DLFS_CONFIG=littlefs_config.h -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only

.PHONY: kernel build run runtime \
        test test-files test-lisp test-tooling test-web test-probes test-memory test-guest test-storage test-network test-network-guest \
        lint check web web-assets web-serve test-browser test-browser-network help
.NOTPARALLEL: check
BUILD_IMAGE := emacsos-kernel:alpine3.22
KERNEL_C := main hardware memory loader runtime files file_abi block storage_control littlefs_backend network ne2000
KERNEL_ASM := entry interrupts syscall
KERNEL_HEADERS := $(wildcard kernel/*.h kernel/arch/*.h kernel/lwip-libc/*.h) \
                  $(wildcard third_party/littlefs/*.h) $(shell find third_party/lwip/src/include -name "*.h" 2>/dev/null)
LWIP_SOURCES := $(addprefix core/,$(addsuffix .c,init def inet_chksum ip mem memp netif pbuf stats sys tcp tcp_in tcp_out timeouts)) \
                $(addprefix core/ipv4/,$(addsuffix .c,etharp icmp ip4 ip4_addr)) netif/ethernet.c
KERNEL_OBJS := $(addprefix $(OUT)/,$(addsuffix .o,$(KERNEL_C) $(KERNEL_ASM))) $(OUT)/lfs.o $(addprefix $(OUT)/lwip/,$(LWIP_SOURCES:.c=.o))

$(OUT):
	mkdir -p $@

$(OUT)/%.o: kernel/%.asm Makefile build/toolchains/kernel.id | $(OUT)
	$(NASM) -f elf64 $< -o $@

$(OUT)/%.o: kernel/%.c $(KERNEL_HEADERS) Makefile build/toolchains/kernel.id | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/lwip/%.o: third_party/lwip/src/%.c $(KERNEL_HEADERS) Makefile build/toolchains/kernel.id
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/lfs.o: third_party/littlefs/lfs.c $(KERNEL_HEADERS) Makefile build/toolchains/kernel.id | $(OUT)
	$(CC) $(CFLAGS) -Wno-unused-function -c $< -o $@

$(OUT)/data-image: tools/data-image.c kernel/littlefs_backend.c third_party/littlefs/lfs.c $(KERNEL_HEADERS) Makefile build/toolchains/kernel.id | $(OUT)
	clang -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-function -DEMACSOS_HOST_TEST \
	  -I kernel -I third_party/littlefs -DLFS_CONFIG=littlefs_config.h \
	  tools/data-image.c kernel/littlefs_backend.c third_party/littlefs/lfs.c -o $@

build/image/data-empty.img: $(OUT)/data-image
	mkdir -p $(@D)
	rm -f $@.part
	$(OUT)/data-image $@.part
	mv $@.part $@

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

# Docker owns the shared toolchain cache; each checkout owns its build output.
kernel:
	python3 scripts/image.py --check-dependencies
	mkdir -p build/toolchains
	docker build --provenance=false --platform linux/amd64 --iidfile build/toolchains/kernel.id.part \
	  -t $(BUILD_IMAGE) -f scripts/Dockerfile.build scripts
	@cmp -s build/toolchains/kernel.id.part build/toolchains/kernel.id || \
	  cp build/toolchains/kernel.id.part build/toolchains/kernel.id
	rm -f build/toolchains/kernel.id.part
	docker run --rm --platform linux/amd64 --user "$$(id -u):$$(id -g)" \
	  -e HOME=/tmp -v "$(CURDIR):/work:z" -w /work \
	  "$$(cat build/toolchains/kernel.id)" $(MAKE) $(OUT)/emacsos.img build/image/data-empty.img

run: build
	python3 scripts/qemu.py

runtime:
	./scripts/build-runtime.sh

build: runtime kernel
	python3 scripts/image.py

test: test-files test-network test-lisp test-tooling test-web

test-files:
	python3 tests/kernel/check-files.py

test-network:
	python3 tests/kernel/network-abi.py

test-lisp:
	mkdir -p build/lisp
	emacs -Q --batch -L lisp \
	  --eval '(setq byte-compile-error-on-warn t byte-compile-dest-file-function (lambda (file) (expand-file-name (file-name-nondirectory (concat file "c")) "build/lisp/")))' \
	  -f batch-byte-compile lisp/*.el
	emacs -Q --batch -L lisp -l tests/lisp/emacsos-app-tests.el -f ert-run-tests-batch-and-exit

test-tooling:
	python3 -m unittest discover -s tests/tooling -p 'test_*.py'

test-web:
	python3 -m unittest discover -s tests/web -p 'test_*.py'

lint:
	python3 scripts/check-contribution.py --require-clean-submodule

test-memory: kernel
	python3 tests/kernel/check-memory.py

test-probes: kernel
	python3 tests/kernel/check-memory.py
	python3 tests/kernel/check-probe.py
	python3 tests/kernel/check-storage-device.py
	python3 tests/kernel/check-network.py

test-guest: build
	python3 tests/guest/check.py

test-storage: build
	python3 tests/guest/check.py --storage

test-network-guest: build
	python3 tests/guest/network.py

# Run in order even under make -j: probes and guest acceptance share kernel files.
check: test lint test-probes test-guest test-storage test-network-guest

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

test-browser-network:
	node tests/web/network-browser.cjs '$(WEB_URL)'

help:
	@echo 'make build  - build the complete image using Docker (default)'
	@echo 'make run    - build and launch the native console'
	@echo 'make web    - build, verify browser assets and serve the browser console'
	@echo 'make test   - run host file, network ABI, Lisp, tooling and web server tests'
	@echo 'make check  - run host tests, conventions, kernel probes and guest acceptance'
	@echo 'make test-browser - test an already running web server (optional Playwright)'
	@echo 'make test-browser-network - test guest TCP and retained files in that browser'
