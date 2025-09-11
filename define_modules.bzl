# TODO
# Add ddk module definition for frpc-trusted driver
load("@rules_pkg//pkg:install.bzl", "pkg_install")
load("@rules_pkg//pkg:mappings.bzl", "pkg_files", "strip_prefix")

load(
    "//build/kernel/kleaf:kernel.bzl",
    "ddk_headers",
    "ddk_module",
    "kernel_module",
    "kernel_modules_install",
)

def define_modules(target, variant, build_loader):
    kernel_build_variant = "{}_{}".format(target, variant)
    data = [
        ":{}_frpc-adsprpc".format(kernel_build_variant),]
    if build_loader == "yes":
        data += [
        ":{}_cdsp-loader".format(kernel_build_variant),
        ]

    kernel_build = select({
        "//build/kernel/kleaf:socrepo_true": "//soc-repo:{}_base_kernel".format(kernel_build_variant),
        "//build/kernel/kleaf:socrepo_false": "//msm-kernel:{}".format(kernel_build_variant),
    })
    ddk_deps = select({
        "//build/kernel/kleaf:socrepo_true":[
            "//soc-repo:all_headers",
            "//soc-repo:{}/drivers/firmware/qcom/qcom-scm".format(kernel_build_variant),
            "//soc-repo:{}/drivers/soc/qcom/mem_buf/mem_buf_dev".format(kernel_build_variant),
            "//soc-repo:{}/drivers/soc/qcom/pdr_interface".format(kernel_build_variant),
            "//soc-repo:{}/drivers/rpmsg/qcom_glink".format(kernel_build_variant),
        ],
        "//build/kernel/kleaf:socrepo_false": ["//msm-kernel:all_headers"],
    })

    # Path to dsp folder from soc-repo/include/trace directory
    trace_include_path = "../../../{}/dsp".format(native.package_name())

    ddk_module(
        name = "{}_frpc-adsprpc".format(kernel_build_variant),
        kernel_build = kernel_build,
        deps = ddk_deps,
        srcs = [
            "dsp/fastrpc.c",
            "dsp/fastrpc_rpmsg.c",
            "dsp/fastrpc_shared.h",
            "dsp/fastrpc_trace.h",
            "dsp/fastrpc_sysfs.c"
        ],
        local_defines = ["DSP_TRACE_INCLUDE_PATH={}".format(trace_include_path)],
        out = "frpc-adsprpc.ko",
        hdrs = [
            "include/uapi/misc/fastrpc.h",
            "include/linux/fastrpc.h"
        ],
        includes = [
            "include/linux",
            "include/uapi",
        ],
    )

    if build_loader == "yes":
        ddk_module(
            name = "{}_cdsp-loader".format(kernel_build_variant),
            kernel_build = "//msm-kernel:{}".format(kernel_build_variant),
            deps = ["//msm-kernel:all_headers"],
            srcs = ["dsp/cdsp-loader.c"],
            out = "cdsp-loader.ko",
        )

    pkg_files(
        name = kernel_build_variant + "_dist_files",
        srcs = data,
        visibility = ["//visibility:private"],
        strip_prefix = strip_prefix.files_only(),
    )

    pkg_install(
        name = "{}_dsp-kernel_dist".format(kernel_build_variant),
        srcs = [":{}_dist_files".format(kernel_build_variant)],
        destdir = "out/target/product/{}/dlkm/lib/modules/".format(target),
    )

def define_vm_modules(target, variant):
    kernel_build_variant = "{}_{}".format(target, variant)

    kernel_build = select({
        "//build/kernel/kleaf:socrepo_true": "//soc-repo:{}_base_kernel".format(kernel_build_variant),
        "//build/kernel/kleaf:socrepo_false": "//msm-kernel:{}".format(kernel_build_variant),
    })

    deps = select({
        "//build/kernel/kleaf:socrepo_true": [
            "//soc-repo:all_headers",
            "//soc-repo:{}/drivers/firmware/qcom/qcom-scm".format(kernel_build_variant),
            "//soc-repo:{}/drivers/soc/qcom/mem_buf/mem_buf_dev".format(kernel_build_variant),
            "//soc-repo:{}/drivers/dma-buf/heaps/qcom_dma_heaps".format(kernel_build_variant),
            ] ,
        "//build/kernel/kleaf:socrepo_false": ["//msm-kernel:all_headers"],
    })

    # Path to dsp folder from soc-repo/include/trace directory
    trace_include_path = "../../../{}/dsp".format(native.package_name())

    ddk_module(
        name = "{}_frpc-trusted-adsprpc".format(kernel_build_variant),
        kernel_build = kernel_build,
        deps = deps,
        srcs = [
            "dsp/fastrpc.c",
            "dsp/fastrpc_socket.c",
            "dsp/fastrpc_shared.h",
            "dsp/fastrpc_trace.h",
            "dsp/fastrpc_sysfs.c"
        ],
        local_defines = [
            "DSP_TRACE_INCLUDE_PATH={}".format(trace_include_path),
            "CONFIG_QCOM_FASTRPC_TRUSTED=1"
        ],
        out = "frpc-trusted-adsprpc.ko",
        hdrs = [
            "include/uapi/misc/fastrpc.h",
            "include/linux/fastrpc.h"
        ],
        includes = [
            "include/linux",
            "include/uapi",
        ],
    )

    pkg_files(
        name = kernel_build_variant + "_dist_files",
        srcs = [":{}_frpc-trusted-adsprpc".format(kernel_build_variant)],
        visibility = ["//visibility:private"],
        strip_prefix = strip_prefix.files_only(),
    )

    pkg_install(
        name = "{}_dsp-kernel_dist".format(kernel_build_variant),
        srcs = [":{}_dist_files".format(kernel_build_variant)],
        destdir = "out/target/product/{}/dlkm/lib/modules/".format(target),
    )
