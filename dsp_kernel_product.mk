SOONG_CONFIG_qtidspplatform_hy11:= false
SOONG_CONFIG_qtidspplatform_hy22:= false
SOONG_CONFIG_qtidspplatform_hy33:= false
SOONG_CONFIG_qtidspplatform_hwasan:= false

BUILD_dspplatform_TECHPACK_SOURCE := true

# Soong Key
SOONG_CONFIG_qtidspplatform := hwasan hy11 hy22 hy33

# Soong Value
SOONG_CONFIG_qtidspplatform_hwasan := false

# Soong Namespace
SOONG_CONFIG_NAMESPACES += qtidspplatform

# Soong Keys

ifeq (,$(wildcard $(QCPATH)/adsprpc))
ifneq ($(filter hwaddress,$(SANITIZE_TARGET)),)
SOONG_CONFIG_qtidspplatform_hwasan:= true
endif

ifeq ($(BUILD_dspplatform_TECHPACK_SOURCE), true)
SOONG_CONFIG_qtidspplatform_hy11:= true
SOONG_CONFIG_qtidspplatform_hy22:= true
SOONG_CONFIG_qtidspplatform_hy33:= true
endif
endif
ifeq ($(TARGET_KERNEL_DLKM_DISABLE), true)
  ifeq ($(TARGET_KERNEL_DLKM_FASTRPC_OVERRIDE), false)
    FASTRPC_DLKM_ENABLED := false
  endif
endif

ifeq ($(FASTRPC_DLKM_ENABLED), true)
PRODUCT_PACKAGES += frpc-adsprpc.ko
ifeq ($(TARGET_BOARD_PLATFORM), seraph)
PRODUCT_PACKAGES += cdsp-loader.ko
ADSPRPC_KERNEL := frpc-adsprpc.ko

endif
endif