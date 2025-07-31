include vendor/qcom/opensource/dsp-kernel/dsp_kernel_product.mk
include vendor/qcom/opensource/dsp-kernel/dsp_kernel_board.mk
include vendor/qcom/proprietary/dspservices_ship/dspservices_vendor_product.mk
#include	vendor/qcom/proprietary/adsprpc/src/vendor.qti.cdsprpc-service.rc
.PHONY:	dspplatform_tp dspplatform_vendor_tp dspplatform_kernel_tp dspplatform_vendor_kernel_module_tp dsplatform_debug_tp

dspplatform_tp: dspplatform_vendor_tp dspplatform_kernel_tp dspplatform_vendor_kernel_module_tp dsplatform_debug_tp

dspplatform_vendor_tp: $(ADSPRPC_VENDOR)
dspplatform_kernel_tp: $(ADSPRPC_KERNEL)
dspplatform_vendor_kernel_module_tp: $(BOARD_VENDOR_KERNEL_MODULES)
dsplatform_debug_tp: $(ADSPRPC_DBG)