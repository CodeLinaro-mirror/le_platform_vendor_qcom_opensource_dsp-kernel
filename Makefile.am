DSP_KERNEL_ROOT=$(ROOTDIR)vendor/qcom/opensource/dsp-kernel
ifeq ($(CONFIG_ARCH_SM6150), y)
KBUILD_OPTIONS := DSP_KERNEL_ROOT=$(DSP_KERNEL_ROOT)
else
KBUILD_OPTIONS := DSP_KERNEL_ROOT=$(DSP_KERNEL_ROOT) CONFIG_QCOM_FASTRPC_TRUSTED=m
endif

ifeq ($(TARGET_SUPPORT),genericarmv8)
	KBUILD_OPTIONS += CONFIG_ARCH_PINEAPPLE=y
endif

ifeq ($(TARGET_SUPPORT),genericarmv8)
	KBUILD_OPTIONS += CONFIG_ARCH_SM6150=y
endif

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) INSTALL_MOD_STRIP=1 -C $(KERNEL_SRC) M=$(M) modules_install

%:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@ $(KBUILD_OPTIONS)

clean:
	rm -f *.o *.ko *.mod.c *.mod.o *~ .*.cmd Module.symvers
	rm -rf .tmp_versions
