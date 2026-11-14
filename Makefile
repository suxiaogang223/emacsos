.DEFAULT_GOAL := mvp
CC := clang
LD := ld.lld
OBJCOPY := llvm-objcopy
NASM := nasm
OUT := build/skeleton
CFLAGS := --target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror \
          -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only

.PHONY: image kernel container-image run
KERNEL_C := main hardware memory loader runtime files file_abi
KERNEL_ASM := entry interrupts syscall
KERNEL_OBJS := $(addprefix $(OUT)/,$(addsuffix .o,$(KERNEL_C) $(KERNEL_ASM)))
image: $(OUT)/emacsos.img
kernel: $(OUT)/kernel.bin

$(OUT):
	mkdir -p $@

$(OUT)/%.o: kernel/%.asm Makefile | $(OUT)
	$(NASM) -f elf64 $< -o $@

$(OUT)/%.o: kernel/%.c kernel/kernel.h kernel/files.h Makefile | $(OUT)
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

container-image:
	./scripts/build-container.sh make image

run: mvp
	./scripts/run-qemu.sh

.PHONY: runtime runtime-smoke
runtime:
	./scripts/build-runtime.sh

runtime-smoke:
	python3 scripts/smoke-runtime.py

.PHONY: mvp check-mvp
mvp: runtime container-image
	mkdir -p build/mvp
	python3 scripts/pack-runtime.py $(OUT)/emacsos.img build/runtime/emacs build/runtime/runtime.cpio build/mvp/emacsos.img
	python3 scripts/mvp-manifest.py

check-mvp: mvp
	python3 scripts/check-mvp.py

WEB_HOST ?= 127.0.0.1
WEB_PORT ?= 18082
.PHONY: web web-assets web-serve check-web
web-assets:
	python3 scripts/fetch-web-assets.py

web: mvp web-assets
	python3 scripts/serve-web.py --host $(WEB_HOST) --port $(WEB_PORT)

web-serve: web-assets
	python3 scripts/serve-web.py --host $(WEB_HOST) --port $(WEB_PORT)

check-web:
	python3 -m unittest discover -s scripts/tests -p 'test_web.py'
