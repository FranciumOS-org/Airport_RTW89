# AirPort_RTW89 — Linux rtw89 (RTL8852BE) for macOS
#
# M0 workflow (in the macOS build VM):
#   make fetch-firmware          # once
#   make -k compile              # compile every object, keep going on errors
#   make errors                  # group errors into docs/compile-status.md
#
# The kext link/bundle targets arrive with M1 (see docs/PORTING.md).

HOST_CPUS    := $(shell sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
MAKEFLAGS    += -j$(HOST_CPUS)

# Relative on purpose: VMware shares mount under "/Volumes/VMware Shared Folders/",
# and make cannot cope with spaces in absolute paths. Run make from the repo root.
PROJ_ROOT    := .
RTW89_SRC    := $(PROJ_ROOT)/third_party/rtw89
COMPAT_DIR   := $(PROJ_ROOT)/src/compat
COMPAT89_DIR := $(PROJ_ROOT)/src/compat_rtw89
MKSDK        := $(PROJ_ROOT)/third_party/MacKernelSDK
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

# rtw89 overlay first so its linux/*.h win, then the inherited rtw88 shims.
COMPAT_FLAGS := -I$(COMPAT89_DIR) \
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

# Inherited compat runtime (rtw88_firmware.c/fw_blobs come back with M1)
COMPAT_SRCS := $(COMPAT_DIR)/rtw88_thread_call.c \
               $(COMPAT_DIR)/rtw88_compat.c

COMPAT89_SRCS := $(COMPAT89_DIR)/rtw89_compat.c \
                 $(COMPAT89_DIR)/rtw89_debug_shim.c

DRIVER_OBJS   := $(patsubst %,$(BUILD_DIR)/rtw89/%.o,$(CORE_SRCS) $(CHIP_SRCS))
COMPAT_OBJS   := $(patsubst $(COMPAT_DIR)/%.c,$(BUILD_DIR)/compat/%.o,$(COMPAT_SRCS))
COMPAT89_OBJS := $(patsubst $(COMPAT89_DIR)/%.c,$(BUILD_DIR)/compat_rtw89/%.o,$(COMPAT89_SRCS))
ALL_OBJS      := $(DRIVER_OBJS) $(COMPAT_OBJS) $(COMPAT89_OBJS)

# ------------------------------------------------------------------ #
# Targets                                                              #
# ------------------------------------------------------------------ #

.PHONY: all compile errors fetch-firmware clean

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

errors:
	@python3 tools/errsum.py $(BUILD_DIR)/log docs/compile-status.md

FW_URL := https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/rtw89/rtw8852b_fw-1.bin
fetch-firmware:
	@mkdir -p $(FIRMWARE_DIR)
	curl -fL --output $(FIRMWARE_DIR)/rtw8852b_fw-1.bin $(FW_URL)
	@shasum -a 256 $(FIRMWARE_DIR)/rtw8852b_fw-1.bin

clean:
	rm -rf $(BUILD_DIR)

-include $(ALL_OBJS:.o=.d)
