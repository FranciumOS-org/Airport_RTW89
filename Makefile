# AirPort_RTW89 — Linux rtw89 (RTL8852BE) for macOS
#
# Workflow (run from the repo root):
#   make fetch-firmware          # once
#   make -k compile              # compile every object, keep going on errors
#   make errors                  # group errors into docs/compile-status.md
#   make link                    # partial-link all objects, check what is left
#                                # for the kernel (docs/kernel-imports.md)
#   make hosttest                # run probe()/remove() and the self-test in userspace
#   make kext                    # build/out/AirPort_RTW89.kext (runs hosttest first)
#
# Loading the kext is never done from here; see CLAUDE.md.

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

# Optimise like the Linux build this code is written for: -O0 frames are far
# larger and the kernel stack is 16 KB. The -f flags are the dialect Linux
# compiles drivers in (type punning, wrapping arithmetic, no "this pointer was
# dereferenced, so it cannot be NULL" deductions).
KEXT_FLAGS   := -fno-exceptions -fno-stack-protector -mkernel \
                -O2 -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks \
                -fno-omit-frame-pointer -Wframe-larger-than=1024 \
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

# Every IOLog of the driver also lands in its own log ring (src/kext/rtw89_logring.cpp,
# read with `rtw89ctl log`); the hosttest provides rtw89_iolog as printf.
LOGRING_DEF := -DIOLog=rtw89_iolog

DRIVER_CFLAGS := \
    $(KEXT_FLAGS) $(COMPAT_FLAGS) $(LOGRING_DEF) \
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
# core.c is compiled through src/compat_rtw89/rtw89_core_wrap.c, which includes
# it unchanged and adds one function that needs its static helpers.
CORE_SRCS := mac80211 mac mac_be phy phy_be fw cam efuse efuse_be \
             regd sar coex ps chan ser acpi util pci pci_be

# The chips and their PCIe front ends (the USB ones are left out)
CHIP_SRCS := rtw8851b rtw8851b_table rtw8851b_rfk rtw8851b_rfk_table rtw8851be \
             rtw8852a rtw8852a_table rtw8852a_rfk rtw8852a_rfk_table rtw8852ae \
             rtw8852b_common rtw8852b rtw8852b_table rtw8852b_rfk \
             rtw8852b_rfk_table rtw8852be \
             rtw8852bt rtw8852bt_rfk rtw8852bt_rfk_table rtw8852bte \
             rtw8852c rtw8852c_table rtw8852c_rfk rtw8852c_rfk_table rtw8852ce \
             rtw8922a rtw8922a_rfk rtw8922ae \
             rtw8922d rtw8922d_rfk rtw8922de

# Inherited compat runtime
COMPAT_SRCS := $(COMPAT_DIR)/rtw88_thread_call.c \
               $(COMPAT_DIR)/rtw88_compat.c

# rtw89_cfg80211.c / rtw89_mac80211.c: the code behind the upstream
# net/cfg80211.h and net/mac80211.h that the driver calls.
COMPAT89_SRCS := $(COMPAT89_DIR)/rtw89_compat.c \
                 $(COMPAT89_DIR)/rtw89_cfg80211.c \
                 $(COMPAT89_DIR)/rtw89_cfg80211_bitrate.c \
                 $(COMPAT89_DIR)/rtw89_mac80211.c \
                 $(COMPAT89_DIR)/rtw89_mlme.c \
                 $(COMPAT89_DIR)/rtw89_data.c \
                 $(COMPAT89_DIR)/rtw89_glue.c \
                 $(COMPAT89_DIR)/rtw89_core_wrap.c \
                 $(COMPAT89_DIR)/rtw89_debug_shim.c

DRIVER_OBJS   := $(patsubst %,$(BUILD_DIR)/rtw89/%.o,$(CORE_SRCS) $(CHIP_SRCS))
COMPAT_OBJS   := $(patsubst $(COMPAT_DIR)/%.c,$(BUILD_DIR)/compat/%.o,$(COMPAT_SRCS))
COMPAT89_OBJS := $(patsubst $(COMPAT89_DIR)/%.c,$(BUILD_DIR)/compat_rtw89/%.o,$(COMPAT89_SRCS))

# Firmware loader + the images from firmware/, embedded as zlib blobs. The table
# is empty until `make fetch-firmware` has been run.
GEN_DIR       := $(BUILD_DIR)/gen
FW_BLOBS_C    := $(GEN_DIR)/fw_blobs.c
FW_BINS       := $(wildcard $(FIRMWARE_DIR)/*.bin)
FW_OBJS       := $(BUILD_DIR)/fw/rtw88_firmware.o $(BUILD_DIR)/fw/fw_blobs.o \
                 $(BUILD_DIR)/fw/rtw89_crypto.o
# Plain kernel C: rtw88_firmware.c is written against IOKit, not the Linux shims.
FW_CFLAGS     := $(KEXT_FLAGS) $(LOGRING_DEF) -std=gnu11 -DKERNEL -I$(COMPAT_DIR)

# WPA3: hostap's SAE code on Mbed TLS (third_party/), adapted by
# src/compat_rtw89/rtw89_hostap.c and the stand-in headers in hostap/.
SAE_INC  := -I$(COMPAT89_DIR)/hostap -I$(PROJ_ROOT)/third_party/hostap/src \
            -I$(PROJ_ROOT)/third_party/hostap/src/utils -I$(PROJ_ROOT)/third_party/hostap/src/common \
            -I$(PROJ_ROOT)/third_party/mbedtls/include -I$(PROJ_ROOT)/third_party/mbedtls/library \
            -I$(COMPAT89_DIR) -DMBEDTLS_CONFIG_FILE='"rtw89_mbedtls_config.h"'
SAE_SRCS := third_party/hostap/src/common/sae.c third_party/hostap/src/common/dragonfly.c \
            $(COMPAT89_DIR)/rtw89_wpabuf.c \
            $(addprefix third_party/mbedtls/library/,bignum.c bignum_core.c ecp.c ecp_curves.c \
                constant_time.c platform_util.c) \
            $(COMPAT89_DIR)/rtw89_hostap.c $(COMPAT89_DIR)/rtw89_sae.c
# The same files built for the kernel: plain C with the stand-in libc headers
# (hostap/kernel_libc) in front of the SDK's, the driver's IOLog redirect, and
# no warnings for code that is not ours.
SAE_KFLAGS := $(KEXT_FLAGS) $(LOGRING_DEF) -std=gnu11 -DKERNEL -w \
              -I$(COMPAT89_DIR)/hostap/kernel_libc $(SAE_INC)
SAE_KOBJS  := $(patsubst %.c,$(BUILD_DIR)/sae/%.o,$(notdir $(SAE_SRCS)))

ALL_OBJS      := $(DRIVER_OBJS) $(COMPAT_OBJS) $(COMPAT89_OBJS) $(FW_OBJS) $(SAE_KOBJS)

# ------------------------------------------------------------------ #
# Targets                                                              #
# ------------------------------------------------------------------ #

.PHONY: front deptest all compile errors link hosttest saetest kext fetch-firmware bundle release clean

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

# Self-contained C (no Linux shims), shared with the userspace smoke test.
$(BUILD_DIR)/fw/rtw89_crypto.o: $(COMPAT89_DIR)/rtw89_crypto.c
	@mkdir -p $(dir $@) $(BUILD_DIR)/log
	@echo "  CC   compat_rtw89/rtw89_crypto.c"
	@$(CC) $(FW_CFLAGS) -Wall -c $< -o $@ 2> $(BUILD_DIR)/log/fw_rtw89_crypto.log || { cat $(BUILD_DIR)/log/fw_rtw89_crypto.log >&2; exit 1; }

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

# rtw89 asks for the highest firmware format it knows first (RTW8852B_FW_FORMAT_MAX
# = 2 -> rtw8852b_fw-2.bin) and only then falls back to older ones.
# Userspace smoke test: the same linked object as the kext, with pthread-based
# stand-ins for its kernel imports and a PCI device that does not answer. Run it
# before every kext load; a crash here would have been a panic there.
HOSTTEST := $(BUILD_DIR)/out/hosttest
HOST_OBJS := $(BUILD_DIR)/out/host_main.o $(BUILD_DIR)/out/host_kernel.o $(BUILD_DIR)/out/selftest.o
# Runs one smoke-test binary: $(call hostrun,<binary>,<args>,<log name>)
# PCI IDs of the chips other than the RTL8852BE (b852), one or two per family
OTHER_CHIP_IDS := b851 8852 a85a b85b b520 c852 8922 892b 892d 882d 895d

define hostrun
	@echo "  RUN  $(1) $(2)"
	@perl -e 'alarm 600; exec @ARGV' $(1) $(2) > $(BUILD_DIR)/log/$(3).log 2>&1; \
	    rc=$$?; grep -E '^==|WARN|BUG|HOST:|mlme\]' $(BUILD_DIR)/log/$(3).log; \
	    [ $$rc -eq 0 ] || { echo "  hosttest FAILED (exit $$rc), see $(BUILD_DIR)/log/$(3).log"; exit 1; }
endef

SAETEST  := $(BUILD_DIR)/out/sae_test

vpath %.c third_party/hostap/src/common third_party/mbedtls/library $(COMPAT89_DIR)

$(BUILD_DIR)/sae/%.o: %.c
	@mkdir -p $(dir $@) $(BUILD_DIR)/log
	@echo "  CC   sae/$*.c"
	@$(CC) $(SAE_KFLAGS) -c $< -o $@ 2> $(BUILD_DIR)/log/sae_$*.log || { cat $(BUILD_DIR)/log/sae_$*.log >&2; exit 1; }

# The SAE exchange in userspace, both sides (tools/hosttest/sae_test.c).
saetest:
	@mkdir -p $(BUILD_DIR)/out $(BUILD_DIR)/log
	@cc -O1 -g -w $(SAE_INC) $(SAE_SRCS) $(COMPAT89_DIR)/rtw89_crypto.c tools/hosttest/sae_test.c \
	    -o $(SAETEST) 2> $(BUILD_DIR)/log/sae_test_build.log \
	    || { grep -E 'error' $(BUILD_DIR)/log/sae_test_build.log >&2; exit 1; }
	$(call hostrun,$(SAETEST),,sae_test)

hosttest: link
	@cc -c -O1 -g -Wall -o $(BUILD_DIR)/out/host_kernel.o tools/hosttest/host_kernel.c
	@cc -c -O1 -g -Wall -I$(COMPAT89_DIR) -o $(BUILD_DIR)/out/host_main.o tools/hosttest/host_main.c
	@$(CC) $(DRIVER_CFLAGS) -MF /dev/null -c tools/hosttest/selftest.c -o $(BUILD_DIR)/out/selftest.o 2> $(BUILD_DIR)/log/hosttest_selftest.log \
	    || { grep -E 'error' $(BUILD_DIR)/log/hosttest_selftest.log >&2; exit 1; }
	@$(CC) $(DRIVER_CFLAGS) -MF /dev/null -c tools/hosttest/fakechip_core.c -o $(BUILD_DIR)/out/fakechip_core.o 2> $(BUILD_DIR)/log/hosttest_fakechip_core.log \
	    || { grep -E 'error' $(BUILD_DIR)/log/hosttest_fakechip_core.log >&2; exit 1; }
	@cc $(ARCH) -o $(HOSTTEST) $(HOST_OBJS) $(LINKED_OBJ) -lz
	@cc $(ARCH) -o $(HOSTTEST)_fakechip $(HOST_OBJS) $(BUILD_DIR)/out/fakechip_core.o \
	    $(filter-out $(BUILD_DIR)/compat_rtw89/rtw89_core_wrap.o,$(ALL_OBJS)) -lz
	$(call hostrun,$(HOSTTEST),00,hosttest_00)
	$(call hostrun,$(HOSTTEST),ff,hosttest_ff)
	@# the other chips against a dead device, side by side (each waits out
	@# the chip's power-on timeouts): probe must fail cleanly
	@for id in $(OTHER_CHIP_IDS); do for fill in 00 ff; do \
	    perl -e 'alarm 600; exec @ARGV' $(HOSTTEST) $$fill dead $$id \
	        > $(BUILD_DIR)/log/hosttest_$${id}_$$fill.log 2>&1 \
	        || echo "  hosttest FAILED for 10ec:$$id ($$fill), see $(BUILD_DIR)/log/hosttest_$${id}_$$fill.log" \
	        > $(BUILD_DIR)/log/hosttest_$${id}_$$fill.failed & \
	done; done; wait
	@for id in $(OTHER_CHIP_IDS); do \
	    echo "  RUN  10ec:$$id dead device: probe returned $$(grep -m1 'probe returned' $(BUILD_DIR)/log/hosttest_$${id}_00.log | sed 's/.*probe returned //')"; \
	done
	@if ls $(BUILD_DIR)/log/hosttest_*.failed >/dev/null 2>&1; then \
	    cat $(BUILD_DIR)/log/hosttest_*.failed; rm -f $(BUILD_DIR)/log/hosttest_*.failed; exit 1; fi
	$(call hostrun,$(HOSTTEST)_fakechip,00 ok,hosttest_fakechip)
	@cc -c -O1 -g -w $(filter-out -DMBEDTLS%,$(SAE_INC)) tools/hosttest/sae_test.c -o $(BUILD_DIR)/out/sae_test_main.o
	@cc $(ARCH) -o $(SAETEST)_kernel $(BUILD_DIR)/out/sae_test_main.o $(SAE_KOBJS) \
	    $(BUILD_DIR)/fw/rtw89_crypto.o $(BUILD_DIR)/out/host_kernel.o
	$(call hostrun,$(SAETEST)_kernel,,sae_test_kernel)

# ------------------------------------------------------------------ #
# Kext bundle                                                          #
# ------------------------------------------------------------------ #

# IOKit side: C++ against MacKernelSDK only. It talks to the Linux side through
# src/compat_rtw89/rtw89_glue.h and never sees the compat headers.
KEXT_SRC      := $(PROJ_ROOT)/src/kext
KEXT_CXXFLAGS := $(ARCH) $(MINOS) -isysroot $(SDK) -nostdinc \
                 -std=gnu++17 -O2 -fno-strict-aliasing -mkernel -fapple-kext -fno-rtti \
                 -fno-exceptions \
                 -fno-builtin -fno-common -fno-stack-protector \
                 -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
                 -D__PRIVATE_SPI__ -D__IO80211_TARGET=__MAC_13_0 \
                 -I$(MKSDK)/Headers -I$(COMPAT89_DIR) -Wall -MMD -MP
KEXT_OBJS     := $(BUILD_DIR)/kext/AirPortRTW89.o $(BUILD_DIR)/kext/AirPortRTW89Native.o \
                 $(BUILD_DIR)/kext/rtw89_logring.o \
                 $(BUILD_DIR)/kext/kmod_info.o
KEXT_BUNDLE   := $(BUILD_DIR)/out/AirPort_RTW89.kext
KEXT_BIN      := $(KEXT_BUNDLE)/Contents/MacOS/AirPort_RTW89

$(BUILD_DIR)/kext/%.o: $(KEXT_SRC)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  kext/$*.cpp"
	@$(CXX) $(KEXT_CXXFLAGS) $(LOGRING_DEF) -c $< -o $@

$(BUILD_DIR)/kext/%.o: $(KEXT_SRC)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC   kext/$*.c"
	@$(CC) $(FW_CFLAGS) -c $< -o $@

# The front: the IO80211Controller, injected by OpenCore (src/front/rtw89_front_api.h).
FRONT_SRC    := $(PROJ_ROOT)/src/front
FRONT_BUNDLE := $(BUILD_DIR)/out/AirPortRTW89Front.kext
front:
	@rm -rf $(FRONT_BUNDLE)
	@mkdir -p $(FRONT_BUNDLE)/Contents/MacOS $(BUILD_DIR)/front
	@cp $(FRONT_SRC)/Info.plist $(FRONT_BUNDLE)/Contents/Info.plist
	@echo "  CXX  front/AirPortRTW89Front.cpp"
	@$(CXX) $(KEXT_CXXFLAGS) -D__IO80211_TARGET=__MAC_13_0 -Wno-inconsistent-missing-override \
	    -c $(FRONT_SRC)/AirPortRTW89Front.cpp -o $(BUILD_DIR)/front/AirPortRTW89Front.o
	@$(CC) $(FW_CFLAGS) -c $(FRONT_SRC)/kmod_info.c -o $(BUILD_DIR)/front/kmod_info.o
	@$(CXX) $(ARCH) $(MINOS) -isysroot $(SDK) -nostdlib -Xlinker -kext \
	    -L$(MKSDK)/Library/x86_64 $(BUILD_DIR)/front/AirPortRTW89Front.o \
	    $(BUILD_DIR)/front/kmod_info.o -lkmod -lcc_kext \
	    -o $(FRONT_BUNDLE)/Contents/MacOS/AirPortRTW89Front
	@codesign --force --sign - $(FRONT_BUNDLE) 2>/dev/null || true
	@echo "  KEXT $(FRONT_BUNDLE)"

# A kext that does nothing but depend on IO80211FamilyLegacy, for tools/deptest.sh.
DEPTEST_BUNDLE := $(BUILD_DIR)/out/RTW89DepTest.kext
deptest:
	@rm -rf $(DEPTEST_BUNDLE)
	@mkdir -p $(DEPTEST_BUNDLE)/Contents/MacOS $(BUILD_DIR)/kext
	@cp tools/deptest/Info.plist $(DEPTEST_BUNDLE)/Contents/Info.plist
	@$(CC) $(FW_CFLAGS) -c tools/deptest/deptest.c -o $(BUILD_DIR)/kext/deptest.o
	@$(CXX) $(ARCH) $(MINOS) -isysroot $(SDK) -nostdlib -Xlinker -kext \
	    -L$(MKSDK)/Library/x86_64 $(BUILD_DIR)/kext/deptest.o -lkmod -lcc_kext \
	    -o $(DEPTEST_BUNDLE)/Contents/MacOS/RTW89DepTest
	@codesign --force --sign - $(DEPTEST_BUNDLE) 2>/dev/null || true
	@echo "  KEXT $(DEPTEST_BUNDLE)"

# The smoke test gates the bundle: nothing gets packaged that crashed there.
kext: hosttest $(KEXT_OBJS) $(KEXT_SRC)/Info.plist
	@rm -rf $(BUILD_DIR)/out/AirPort_RTW89.kext
	@mkdir -p $(dir $(KEXT_BIN))
	@cp $(KEXT_SRC)/Info.plist $(KEXT_BUNDLE)/Contents/Info.plist
	@$(CXX) $(ARCH) $(MINOS) -isysroot $(SDK) -nostdlib -Xlinker -kext \
	    -L$(MKSDK)/Library/x86_64 $(ALL_OBJS) $(KEXT_OBJS) -lkmod -lcc_kext -o $(KEXT_BIN)
	@codesign --force --sign - $(KEXT_BUNDLE) 2>/dev/null || true
	@echo "  KEXT $(KEXT_BUNDLE)"
	@python3 tools/check_kpi.py $(KEXT_BUNDLE)
	@cc -O2 -Wall -o $(BUILD_DIR)/out/rtw89ctl tools/rtw89ctl.c -framework IOKit -framework CoreFoundation
	@echo "  TOOL $(BUILD_DIR)/out/rtw89ctl"

# Realtek's firmware for each chip, the newest format its driver takes, and
# Realtek's terms for redistributing it; linux-firmware at a fixed commit.
FW_COMMIT := d947e4e8e314e9254a1242dc1a5d9cede2cce33d
FW_NAMES  := rtw8851b_fw-1.bin rtw8852a_fw-1.bin rtw8852b_fw-2.bin rtw8852bt_fw.bin \
             rtw8852c_fw-2.bin rtw8922a_fw-4.bin rtw8922d_fw.bin rtw8922ds_fw.bin
FW_BASE   := https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain

fetch-firmware:
	@mkdir -p $(FIRMWARE_DIR)
	@for f in $(FW_NAMES); do \
	    echo "  GET  rtw89/$$f"; \
	    curl -fsSL -m 300 --output $(FIRMWARE_DIR)/$$f "$(FW_BASE)/rtw89/$$f?id=$(FW_COMMIT)" || exit 1; \
	done
	@curl -fsSL -m 60 --output $(FIRMWARE_DIR)/LICENCE.rtlwifi_firmware.txt \
	    "$(FW_BASE)/LICENSES/LICENCE.rtlwifi_firmware.txt?id=$(FW_COMMIT)"
	@cd $(FIRMWARE_DIR) && shasum -a 256 -c SHA256SUMS

# The kext as it is installed: the driver, with the front inside it as a
# plugin (OpenCore loads Contents/PlugIns/ kexts as entries of their own, after
# IO80211FamilyLegacy) and the licences that must travel with the firmware and
# the GPL code in Contents/Resources. build/out/bundle/AirPort_RTW89.kext.
BUNDLE := $(BUILD_DIR)/out/bundle/AirPort_RTW89.kext

bundle: kext front
	@rm -rf $(BUNDLE)
	@mkdir -p $(dir $(BUNDLE))
	@cp -R $(BUILD_DIR)/out/AirPort_RTW89.kext $(BUNDLE)
	@mkdir -p $(BUNDLE)/Contents/PlugIns $(BUNDLE)/Contents/Resources
	@cp -R $(BUILD_DIR)/out/AirPortRTW89Front.kext $(BUNDLE)/Contents/PlugIns/
	@cp LICENSE CREDITS.md $(FIRMWARE_DIR)/LICENCE.rtlwifi_firmware.txt $(BUNDLE)/Contents/Resources/
	@git rev-parse --short HEAD > $(BUNDLE)/Contents/Resources/COMMIT
	@# the front must start before the driver on the card (see its Info.plist)
	@f=$$(/usr/libexec/PlistBuddy -c 'Print :IOKitPersonalities:RTW89:IOProbeScore' $(BUNDLE)/Contents/PlugIns/AirPortRTW89Front.kext/Contents/Info.plist 2>/dev/null || echo 0); \
	 d=$$(/usr/libexec/PlistBuddy -c 'Print :IOKitPersonalities:RTW89:IOProbeScore' $(BUNDLE)/Contents/Info.plist 2>/dev/null || echo 0); \
	 [ "$$f" -gt "$$d" ] || { echo "  the front's IOProbeScore ($$f) must be above the driver's ($$d)"; exit 1; }
	@echo "  BUNDLE $(BUNDLE) (the front in Contents/PlugIns)"

# The downloads: AirPort_RTW89-<version>.zip, the kext and nothing else; and
# AirPort_RTW89-<version>-tools.zip for testers: the installer (not
# efi/tuf-a15/, this machine's own), rtw89ctl, the log collector, README and
# TESTING.md. The old Wi-Fi stack is not Apple's to give away here.
VERSION     := $(shell /usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' src/kext/Info.plist)
RELEASE     := AirPort_RTW89-$(VERSION)
TOOLS       := $(RELEASE)-tools
TOOLS_DIR   := $(BUILD_DIR)/release/$(TOOLS)

release: bundle
	@rm -rf $(BUILD_DIR)/release
	@mkdir -p $(TOOLS_DIR)/efi/kit $(TOOLS_DIR)/tools
	@cp -R $(BUNDLE) $(BUILD_DIR)/release/
	@cp efi/install.sh efi/uninstall.sh efi/configure.py efi/find_efi.sh efi/README.md $(TOOLS_DIR)/efi/
	@printf 'Put IOSkywalkFamily.kext, IO80211FamilyLegacy.kext and AMFIPass.kext here:\nsee ../README.md, "Requirements".\n' > $(TOOLS_DIR)/efi/kit/PUT-KEXTS-HERE.txt
	@printf 'Put AirPort_RTW89.kext (from %s.zip) in this folder, next to this file.\n' $(RELEASE) > $(TOOLS_DIR)/PUT-AirPort_RTW89.kext-HERE.txt
	@cp $(BUILD_DIR)/out/rtw89ctl tools/collect_logs.sh $(TOOLS_DIR)/tools/
	@cp setup.py setup.command setup.cmd $(TOOLS_DIR)/
	@cp README.md TESTING.md LICENSE $(TOOLS_DIR)/
	@find $(BUILD_DIR)/release -name '.DS_Store' -delete
	@cd $(BUILD_DIR)/release && COPYFILE_DISABLE=1 zip -qry $(RELEASE).zip AirPort_RTW89.kext && \
	    COPYFILE_DISABLE=1 zip -qry $(TOOLS).zip $(TOOLS)
	@echo "  ZIP  $(BUILD_DIR)/release/$(RELEASE).zip ($$(du -h $(BUILD_DIR)/release/$(RELEASE).zip | cut -f1)): AirPort_RTW89.kext, commit $$(cat $(BUNDLE)/Contents/Resources/COMMIT)"
	@echo "  ZIP  $(BUILD_DIR)/release/$(TOOLS).zip ($$(du -h $(BUILD_DIR)/release/$(TOOLS).zip | cut -f1))"

clean:
	rm -rf $(BUILD_DIR)

-include $(ALL_OBJS:.o=.d) $(KEXT_OBJS:.o=.d)
