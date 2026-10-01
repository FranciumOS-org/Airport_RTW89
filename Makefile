# AirPort_RTW89 — Linux rtw89 (RTL8852BE) for macOS
#
# Workflow (run from the repo root):
#   make fetch-firmware          # once
#   make -k compile              # compile every object, keep going on errors
#   make errors                  # group errors into docs/compile-status.md
#   make link                    # partial-link all objects, check what is left
#                                # for the kernel (docs/kernel-imports.md)
#
# The kext bundle target arrives with M1 (see docs/PORTING.md).

HOST_CPUS    := $(shell sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
MAKEFLAGS    += -j$(HOST_CPUS)

# Relative on purpose: make cannot cope with spaces in absolute paths.
PROJ_ROOT    := .
RTW89_SRC    := $(PROJ_ROOT)/third_party/rtw89
COMPAT_DIR   := $(PROJ_ROOT)/src/compat
COMPAT89_DIR := $(PROJ_ROOT)/src/compat_rtw89
MKSDK        := $(PROJ_ROOT)/third_party/MacKernelSDK
LINUX_INC    := $(PROJ_ROOT)/third_party/linux-include
FIRMWARE_DIR := $(PROJ_ROOT)/firmware
BUILD_DIR    := $(PROJ_ROOT)/build

SDK          := $(shell xcrun --show-sdk-path 2>/dev/null)
ARCH         := -arch x86_64
MINOS        := -mmacosx-version-min=13.0
CC           := xcrun clang
CXX          := xcrun clang++

KEXT_FLAGS   := -fno-exceptions -fno-stack-protector -mkernel \
                -MMD -MP \
                $(ARCH) $(MINOS) \
                -isysroot $(SDK) \
                -I$(MKSDK)/Headers \
                -I$(SDK)/System/Library/Frameworks/Kernel.framework/Headers

# rtw89 overlay first so its linux/*.h win, then the unmodified upstream
# 802.11 headers (ieee80211, nl80211, cfg80211, mac80211), then the inherited
# rtw88 shims for everything below them.
COMPAT_FLAGS := -I$(COMPAT89_DIR) \
                -I$(LINUX_INC) \
                -I$(LINUX_INC)/uapi \
                -I$(COMPAT_DIR) \
                -I$(COMPAT_DIR)/linux

DRIVER_CFLAGS := \
    $(KEXT_FLAGS) $(COMPAT_FLAGS) \
    -include $(COMPAT89_DIR)/rtw89_compat.h \
    -I$(RTW89_SRC) \
    -std=gnu11 \
    -DRTW89_MACOS=1 -DRTW88_MACOS=1 -D__KERNEL__ \
    -DCONFIG_RTW89_DEBUGMSG=1 \
    -Werror=implicit-function-declaration \
    -Werror=int-conversion \
    -Werror=incompatible-pointer-types \
    -Wno-unused-variable -Wno-unused-function \
    -ferror-limit=0

# ------------------------------------------------------------------ #
# Sources                                                              #
# ------------------------------------------------------------------ #

# rtw89_core + rtw89_pci (wow.c excluded until sleep/wake; debug.c replaced
# by src/compat_rtw89/rtw89_debug_shim.c)
CORE_SRCS := core mac80211 mac mac_be phy phy_be fw cam efuse efuse_be \
             regd sar coex ps chan ser acpi util pci pci_be

# RTL8852B chip + RTL8852BE PCIe frontend
CHIP_SRCS := rtw8852b_common rtw8852b rtw8852b_table rtw8852b_rfk \
             rtw8852b_rfk_table rtw8852be

# Inherited compat runtime
COMPAT_SRCS := $(COMPAT_DIR)/rtw88_thread_call.c \
               $(COMPAT_DIR)/rtw88_compat.c

# rtw89_cfg80211.c / rtw89_mac80211.c: the code behind the upstream
# net/cfg80211.h and net/mac80211.h that the driver calls.
COMPAT89_SRCS := $(COMPAT89_DIR)/rtw89_compat.c \
                 $(COMPAT89_DIR)/rtw89_cfg80211.c \
                 $(COMPAT89_DIR)/rtw89_cfg80211_bitrate.c \
                 $(COMPAT89_DIR)/rtw89_mac80211.c \
                 $(COMPAT89_DIR)/rtw89_debug_shim.c

DRIVER_OBJS   := $(patsubst %,$(BUILD_DIR)/rtw89/%.o,$(CORE_SRCS) $(CHIP_SRCS))
COMPAT_OBJS   := $(patsubst $(COMPAT_DIR)/%.c,$(BUILD_DIR)/compat/%.o,$(COMPAT_SRCS))
COMPAT89_OBJS := $(patsubst $(COMPAT89_DIR)/%.c,$(BUILD_DIR)/compat_rtw89/%.o,$(COMPAT89_SRCS))

# Firmware loader + the images from firmware/, embedded as zlib blobs. The table
# is empty until `make fetch-firmware` has been run.
GEN_DIR       := $(BUILD_DIR)/gen
FW_BLOBS_C    := $(GEN_DIR)/fw_blobs.c
FW_BINS       := $(wildcard $(FIRMWARE_DIR)/*.bin)
FW_OBJS       := $(BUILD_DIR)/fw/rtw88_firmware.o $(BUILD_DIR)/fw/fw_blobs.o
# Plain kernel C: rtw88_firmware.c is written against IOKit, not the Linux shims.
FW_CFLAGS     := $(KEXT_FLAGS) -std=gnu11 -DKERNEL -I$(COMPAT_DIR)

ALL_OBJS      := $(DRIVER_OBJS) $(COMPAT_OBJS) $(COMPAT89_OBJS) $(FW_OBJS)

# ------------------------------------------------------------------ #
# Targets                                                              #
# ------------------------------------------------------------------ #

.PHONY: all compile errors link fetch-firmware clean

all: compile

compile: $(ALL_OBJS)
	@echo "  OK   all $(words $(ALL_OBJS)) objects compiled"

# Each object's compiler output also lands in build/log/<name>.log so a
# `make -k` run leaves a complete, per-file error record.
$(BUILD_DIR)/rtw89/%.o: $(RTW89_SRC)/%.c
	@mkdir -p $(dir $@) $(BUILD_DIR)/log
	@echo "  CC   $*.c"
	@$(CC) $(DRIVER_CFLAGS) -c $< -o $@ 2> $(BUILD_DIR)/log/$*.log || { cat $(BUILD_DIR)/log/$*.log >&2; exit 1; }

$(BUILD_DIR)/compat/%.o: $(COMPAT_DIR)/%.c
	@mkdir -p $(dir $@) $(BUILD_DIR)/log
	@echo "  CC   compat/$*.c"
	@$(CC) $(DRIVER_CFLAGS) -c $< -o $@ 2> $(BUILD_DIR)/log/compat_$*.log || { cat $(BUILD_DIR)/log/compat_$*.log >&2; exit 1; }

$(BUILD_DIR)/compat_rtw89/%.o: $(COMPAT89_DIR)/%.c
	@mkdir -p $(dir $@) $(BUILD_DIR)/log
	@echo "  CC   compat_rtw89/$*.c"
	@$(CC) $(DRIVER_CFLAGS) -c $< -o $@ 2> $(BUILD_DIR)/log/compat89_$*.log || { cat $(BUILD_DIR)/log/compat89_$*.log >&2; exit 1; }

$(FW_BLOBS_C): $(FW_BINS) tools/gen_fw_blobs.py
	@python3 tools/gen_fw_blobs.py $(FIRMWARE_DIR) $@

$(BUILD_DIR)/fw/rtw88_firmware.o: $(COMPAT_DIR)/rtw88_firmware.c
	@mkdir -p $(dir $@) $(BUILD_DIR)/log
	@echo "  CC   compat/rtw88_firmware.c"
	@$(CC) $(FW_CFLAGS) -c $< -o $@ 2> $(BUILD_DIR)/log/fw_rtw88_firmware.log || { cat $(BUILD_DIR)/log/fw_rtw88_firmware.log >&2; exit 1; }

$(BUILD_DIR)/fw/fw_blobs.o: $(FW_BLOBS_C)
	@mkdir -p $(dir $@) $(BUILD_DIR)/log
	@echo "  CC   gen/fw_blobs.c"
	@$(CC) $(FW_CFLAGS) -c $< -o $@ 2> $(BUILD_DIR)/log/fw_fw_blobs.log || { cat $(BUILD_DIR)/log/fw_fw_blobs.log >&2; exit 1; }

errors:
	@python3 tools/errsum.py $(BUILD_DIR)/log docs/compile-status.md

# Everything except the IOKit glue in one relocatable object. Fails if a symbol
# is left that the kernel does not have, i.e. a Linux function nobody provides.
LINKED_OBJ := $(BUILD_DIR)/out/AirPort_RTW89.o
link: compile
	@mkdir -p $(dir $(LINKED_OBJ))
	@ld -r $(ARCH) -o $(LINKED_OBJ) $(ALL_OBJS)
	@python3 tools/check_imports.py $(LINKED_OBJ) docs/kernel-imports.md

FW_URL := https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/rtw89/rtw8852b_fw-1.bin
fetch-firmware:
	@mkdir -p $(FIRMWARE_DIR)
	curl -fL --output $(FIRMWARE_DIR)/rtw8852b_fw-1.bin $(FW_URL)
	@shasum -a 256 $(FIRMWARE_DIR)/rtw8852b_fw-1.bin

clean:
	rm -rf $(BUILD_DIR)

-include $(ALL_OBJS:.o=.d)
