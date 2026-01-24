FASTRPC_DLKM_ENABLED := true
TARGET_USES_CDSP_LOADER := false

ifeq ($(TARGET_KERNEL_DLKM_DISABLE), true)
	ifeq ($(TARGET_KERNEL_DLKM_FASTRPC_OVERRIDE), false)
		FASTRPC_DLKM_ENABLED := false
	endif
endif

ifeq ($(FASTRPC_DLKM_ENABLED), true)
ifeq ($(call is-board-platform-in-list,$(TARGET_BOARD_PLATFORM)),true)
BOARD_VENDOR_KERNEL_MODULES += $(KERNEL_MODULES_OUT)/frpc-adsprpc.ko
ifeq ($(TARGET_BOARD_PLATFORM), seraph)
# ----- kernel version based cdsp-loader.ko enablement: < 6.6 only -----
KVER_MAJOR := $(word 1,$(subst ., ,$(strip $(TARGET_KERNEL_VERSION))))
KVER_MINOR := $(word 2,$(subst ., ,$(strip $(TARGET_KERNEL_VERSION))))

ifeq ($(call math_lt,$(KVER_MAJOR),6),true)
TARGET_USES_CDSP_LOADER := true
else ifeq ($(KVER_MAJOR),6)
ifeq ($(call math_lt,$(KVER_MINOR),6),true)
TARGET_USES_CDSP_LOADER := true
endif
endif
# -----------------------------------------------

ifeq ($(TARGET_USES_CDSP_LOADER), true)
BOARD_VENDOR_KERNEL_MODULES += $(KERNEL_MODULES_OUT)/cdsp-loader.ko
endif
endif
endif
endif
