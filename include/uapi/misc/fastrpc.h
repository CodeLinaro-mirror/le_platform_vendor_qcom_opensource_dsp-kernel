/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */

#ifndef __QCOM_FASTRPC_H__
#define __QCOM_FASTRPC_H__

#include <linux/types.h>

#define FASTRPC_IOCTL_ALLOC_DMA_BUFF	_IOWR('R', 1, struct fastrpc_alloc_dma_buf)
#define FASTRPC_IOCTL_FREE_DMA_BUFF	_IOWR('R', 2, __u32)
#define FASTRPC_IOCTL_INVOKE		_IOWR('R', 3, struct fastrpc_invoke)
#define FASTRPC_IOCTL_INIT_ATTACH	_IO('R', 4)
#define FASTRPC_IOCTL_INIT_CREATE	_IOWR('R', 5, struct fastrpc_init_create)
#define FASTRPC_IOCTL_MMAP		_IOWR('R', 6, struct fastrpc_req_mmap)
#define FASTRPC_IOCTL_MUNMAP		_IOWR('R', 7, struct fastrpc_req_munmap)
#define FASTRPC_IOCTL_INIT_ATTACH_SNS	_IO('R', 8)
#define FASTRPC_IOCTL_INIT_CREATE_STATIC _IOWR('R', 9, struct fastrpc_init_create_static)
#define FASTRPC_IOCTL_MEM_MAP		_IOWR('R', 10, struct fastrpc_mem_map)
#define FASTRPC_IOCTL_MEM_UNMAP		_IOWR('R', 11, struct fastrpc_mem_unmap)
#define FASTRPC_IOCTL_MULTIMODE_INVOKE	_IOWR('R', 12, struct fastrpc_ioctl_multimode_invoke)
#define FASTRPC_IOCTL_GET_DSP_INFO	_IOWR('R', 13, struct fastrpc_ioctl_capability)
#define FASTRPC_IOCTL_INIT_ATTACH2 	_IOWR('R', 14, struct fastrpc_ioctl_init_attach2)
#define FASTRPC_IOCTL_GET_TIMELINE_BUFFER	_IOWR('R', 15, struct fastrpc_ioctl_timeline_buf)
#define FASTRPC_IOCTL_NPU_PRIORITY_WORKINFO	_IOWR('R', 16, struct fastrpc_ioctl_npu_priority_workinfo)

/* Reserved fields in mdxtx ioctl structs for 64-bit alignment */
#define FASTRPC_MDCTX_IOCTL_RSVD 8

/* Reserved fields in fastrpc_internal_proc_timeout ioctl structs */
#define FASTRPC_RPC_TIMEOUT_IOCTL_RSVD 5

/* Reserved space for timeline arguments structs */
#define FASTRPC_TIMELINE_ARGS_RSVD 8

/* Reserved fields in timeline buffer ioctl structs */
#define FASTRPC_IOCTL_TIMELINE_BUF_RSVD 8

/* Version constants for struct npu_work_info */
#define NPU_WORKINFO_VERSION   0

/**
 * enum fastrpc_map_flags - control flags for mapping memory on DSP user process
 * @FASTRPC_MAP_STATIC: Map memory pages with RW- permission and CACHE WRITEBACK.
 * The driver is responsible for cache maintenance when passed
 * the buffer to FastRPC calls. Same virtual address will be
 * assigned for subsequent FastRPC calls.
 * @FASTRPC_MAP_RESERVED: Reserved
 * @FASTRPC_MAP_FD: Map memory pages with RW- permission and CACHE WRITEBACK.
 * Mapping tagged with a file descriptor. User is responsible for
 * CPU and DSP cache maintenance for the buffer. Get virtual address
 * of buffer on DSP using HAP_mmap_get() and HAP_mmap_put() APIs.
 * @FASTRPC_MAP_FD_DELAYED: Mapping delayed until user call HAP_mmap() and HAP_munmap()
 * functions on DSP. It is useful to map a buffer with cache modes
 * other than default modes. User is responsible for CPU and DSP
 * cache maintenance for the buffer.
 * @FASTRPC_MAP_FD_NOMAP: This flag is used to skip CPU mapping,
 * otherwise behaves similar to FASTRPC_MAP_FD_DELAYED flag.
 * @FASTRPC_MAP_MAX: max count for flags
 *
 */
enum fastrpc_map_flags {
	FASTRPC_MAP_STATIC = 0,
	FASTRPC_MAP_RESERVED,
	FASTRPC_MAP_FD = 2,
	FASTRPC_MAP_FD_DELAYED,
	FASTRPC_MAP_FD_NOMAP = 16,
	FASTRPC_MAP_FD_EXTENDED,
	FASTRPC_MAP_FD_DELAYED_EXTENDED,
	FASTRPC_MAP_MAX,
};

/**
 * @enum fastrpc_map_attrs - attributes for mapping and unmapping
 * memory on DSP user process
 */
enum fastrpc_map_attrs {
	/** Default attribute for all map and unmap requests */
	FASTRPC_MAP_ATTR_DEFAULT = 0,

	/** Attribute for memory protection of buffers */
	FASTRPC_ATTR_SECUREMAP = 1,

	/** Attributes for buffers with no virtual address */
	FASTRPC_ATTR_NOVA = 256,

	/**
	 * When set during buffer mapping, it reserves the IOVA region so it
	 * remains reserved after unmapping also.
	 * When set during unmapping, it removes only the IOMMU mapping while
	 * keeping the IOVA reserved for future reuse during subsequent
	 * mappings of the same buffer.
	 */
	FASTRPC_MAP_ATTR_RETAIN_IOVA = 1024,

	/** Always keep this as the last member */
	FASTRPC_MAP_ATTR_MAX,
};

/* Types of DSP available */
enum fastrpc_dsp_type {
	FASTRPC_NSP =  1,
	FASTRPC_LPASS,
	FASTRPC_SDSP,
	FASTRPC_MAX_DSP_TYPE,
};

enum fastrpc_proc_attr {
	/* Macro for Debug attr */
	FASTRPC_MODE_DEBUG		= (1 << 0),
	/* Macro for Ptrace */
	FASTRPC_MODE_PTRACE		= (1 << 1),
	/* Macro for CRC Check */
	FASTRPC_MODE_CRC		= (1 << 2),
	/* Macro for Unsigned PD */
	FASTRPC_MODE_UNSIGNED_MODULE	= (1 << 3),
	/* Macro for Adaptive QoS */
	FASTRPC_MODE_ADAPTIVE_QOS	= (1 << 4),
	/* Macro for System Process */
	FASTRPC_MODE_SYSTEM_PROCESS	= (1 << 5),
	/* Macro for Prvileged Process */
	FASTRPC_MODE_PRIVILEGED		= (1 << 6),
	/* Macro for system unsigned PD */
	FASTRPC_MODE_SYSTEM_UNSIGNED_PD	= 1 << 17,
};

/* Operation selector for FASTRPC_IOCTL_NPU_PRIORITY_WORKINFO */
enum fastrpc_npu_priority_workinfo_op {
	FASTRPC_NPU_OP_PRIORITY   = 0,
	FASTRPC_NPU_OP_WORKINFO   = 1,
};

/* Event type for FASTRPC_INVOKE_NPU_WORKINFO */
enum npu_work_event {
	WORK_REQUESTED = 0,
	WORK_STARTED   = 1,
	WORK_ENDED     = 2,
};

/*
 * Reason code carried in npu_work_info.reason.
 *
 * The AIDL interface (ISchedulingCallback) exposes onWorkStarted(WorkInfo,
 * StartReason) and onWorkEnded(WorkInfo, EndReason), requiring the HAL to
 * know the specific reason for each event.  Without this field the HAL must
 * hardcode EndReason::COMPLETED for every WORK_ENDED event, silently
 * mislabelling cancelled or preempted work and preventing clients from
 * triggering correct rescheduling logic.  The field occupies the four bytes
 * of implicit compiler padding between the version field (offset 64) and the
 * reserved array (offset 72), leaving the struct layout and total size
 * (192 bytes) unchanged.
 *
 * For WORK_REQUESTED: always NPU_WORK_REASON_NONE.
 * For WORK_STARTED:   NPU_WORK_REASON_START_INITIAL.
 * For WORK_ENDED:     NPU_WORK_REASON_END_COMPLETED or NPU_WORK_REASON_END_CANCELLED.
 */
enum npu_work_reason {
	NPU_WORK_REASON_NONE          = 0, /* WORK_REQUESTED: reason not applicable */
	NPU_WORK_REASON_START_INITIAL = 1, /* WORK_STARTED: first execution, no prior context */
	NPU_WORK_REASON_END_COMPLETED = 2, /* WORK_ENDED: all submitted work completed */
	NPU_WORK_REASON_END_CANCELLED = 3, /* WORK_ENDED: preempted or cancelled by system */
};

struct fastrpc_invoke_args {
	__u64 ptr;
	__u64 length;
	__s32 fd;
	__u32 attr;
};

struct fastrpc_invoke {
	__u32 handle;
	__u32 sc;
	__u64 args;
};

struct fastrpc_enhanced_invoke {
	struct fastrpc_invoke inv;
	__u64 crc;
	__u64 perf_kernel;
	__u64 perf_dsp;
	__u32 priority;
	__u32 appid;
	__u32 reserved[18];
};

struct fastrpc_ioctl_multimode_invoke {
	__u32 req;	/* enum fastrpc_multimode_invoke_type */
	__u64 invparam;
	__u64 size;
	/* Flag to notify if dynamic domain discovery is enabled */
	__u32 dynamic_domains;
	__u32 reserved[7];
};

/* Payload for FASTRPC_IOCTL_NPU_PRIORITY_WORKINFO */
struct fastrpc_npu_priority {
	/* Number of priority config entries */
	__u32 num_configs;

	/*
	 * Version of the config struct pointed to by 'configs'.
	 * Set to NPU_APP_PRIO_CONFIG_VERSION.
	 */
	__u32 version;

	/* User-addr to array of fastrpc_npu_app_prio_config */
	__u64 configs;

	/* Reserved for future use */
	__u32 reserved[8];
};

/*
 * NPU Work Information
 * Corresponds to android.hardware.npu.WorkInfo
 */
struct npu_work_info {
	/* CLOCK_MONOTONIC timestamp in milliseconds at the time of the event */
	__u64  timestamp_ms;

	/*
	 * Userspace pointer to group ID UUID bytes.  The HAL pre-allocates an
	 * output buffer and passes its address here; the kernel copies the UUID
	 * bytes via copy_to_user() and patches this field to carry the userspace
	 * VA on return.  Zero if no group ID is associated with this work item.
	 */
	__u64 group_id;

	/*
	 * Userspace pointer to the debug feature ID string.  Same delivery
	 * mechanism as group_id.  Zero if no debug feature ID is set.
	 */
	__u64 debug_feature_id;

	/* enum npu_work_event: WORK_REQUESTED, WORK_STARTED, or WORK_ENDED */
	__u32 event;

	/* Monotonically increasing work identifier assigned by the kernel */
	__s32  id;

	/* UID of the application that submitted this work item */
	__s32  uid;

	/* PID of the submitting thread, for debugging */
	__s32  debug_pid;

	/* Original UID before any UID attribution was applied */
	__s32  original_uid;

	/* DSP domain identifier; maps to WorkInfo.deviceNumber in the AIDL layer */
	__s32  domain;

	/* Job-level scheduling priority assigned by the submitter */
	__s32  job_priority;

	/*
	 * Effective scheduling priority combining uid-level and job-level
	 * priority to determine ordering relative to other clients.
	 */
	__s32  effective_priority;

	/* Length in bytes of the group_id UUID; 0 if group_id is not set */
	__u32 group_id_len;

	/* Length in bytes of the debug_feature_id string; 0 if not set */
	__u32 debug_feature_id_len;

	/* Struct version; set to NPU_WORKINFO_VERSION */
	__u32 version;

	/*
	 * Reason for this event; see enum npu_work_reason.
	 * NPU_WORK_REASON_NONE for WORK_REQUESTED,
	 * NPU_WORK_REASON_START_* for WORK_STARTED,
	 * NPU_WORK_REASON_END_* for WORK_ENDED.
	 */
	__u32 reason;

	/* Reserved for future use; total struct size = 192 bytes */
	__u64 reserved[15];
};

/* Combined payload for FASTRPC_IOCTL_NPU_PRIORITY_WORKINFO */
struct fastrpc_ioctl_npu_priority_workinfo {
	__u32 op;	/* enum fastrpc_npu_priority_workinfo_op */
	__u32 rsvd; /* padding for alignment */
	union {
		struct fastrpc_npu_priority prio;
		struct npu_work_info workinfo;
	};
	__u32 reserved[16];
};

enum fastrpc_multimode_invoke_type {
	FASTRPC_INVOKE			= 1,
	FASTRPC_INVOKE_ENHANCED	= 2,
	FASTRPC_INVOKE_CONTROL = 3,
	FASTRPC_INVOKE_DSPSIGNAL = 4,
	FASTRPC_INVOKE_NOTIF = 5,
	FASTRPC_INVOKE_MULTISESSION = 6,
	FASTRPC_INVOKE_CONFIG = 7,
	FASTRPC_INVOKE_SESSIONINFO = 8,
	FASTRPC_INVOKE_MDCTX_MANAGE = 9,
	FASTRPC_INVOKE_REMOTE_PROCESS_STATE_DUMP = 10,
	FASTRPC_INVOKE_SET_RPC_TIMEOUT = 11,
	FASTRPC_INVOKE_DISABLE_DSP_RECOVERY = 12,
	FASTRPC_INVOKE_RETRIEVE_KERNEL_LOG = 13,
	FASTRPC_INVOKE_THREAD_EXIT = 14,
	FASTRPC_SET_TIMELINE_INFO = 15,
	FASTRPC_GET_TIMELINE_VERSION = 16,
	FASTRPC_INVOKE_REMOTE_WORK    = 17,
};

/* Status types for FASTRPC_INVOKE_REMOTE_WORK */
enum fastrpc_remote_work_status {
	FASTRPC_REMOTE_WORK_STATUS_START  = 1,
	FASTRPC_REMOTE_WORK_STATUS_END    = 2,
};

struct fastrpc_init_create {
	__u32 filelen;	/* elf file length */
	__s32 filefd;	/* fd for the file */
	__u32 attrs;
	__u32 siglen;
	__u64 file;	/* pointer to elf file */
};

/* payload for FASTRPC_SET_TIMELINE_INFO type */
struct fastrpc_timeline_arguments {
	/**
	 * Number of events the timeline buffer can hold.
	 * Set to 0 if timeline is not supported.
	 */
	__u32 num_events;
	/**
	 * (In) FastRPC timeline version. Negotiates minimum
	 * supported version between HLOS and kernel during init.
	 */
	__u32 version;
	/* Reserved space */
	__u64 reserved[FASTRPC_TIMELINE_ARGS_RSVD];
};

struct fastrpc_ioctl_timeline_buf {
	/* User buf to copy timeline events */
	__u64 addr;
	/* Max timeline events stored in buf */
	__u32 total_events_cnt;
	/* Pointing to current write index */
	__u32 buf_write_index;
	/* Reserved space */
	__u64 reserved[FASTRPC_IOCTL_TIMELINE_BUF_RSVD];
};

struct fastrpc_init_create_static {
	__u32 namelen;	/* length of pd process name */
	__u32 memlen;
	__u64 name;	/* pd process name */
};

struct fastrpc_ioctl_init_attach2 {
	__u32 filelen;	/* elf file length */
	__s32 filefd;	/* fd for the file */
	__u64 file;	/* pointer to elf file */
	__s32 reserved[4];
};

struct fastrpc_alloc_dma_buf {
	__s32 fd;	/* fd */
	__u32 flags;	/* flags to map with */
	__u64 size;	/* size */
};

struct fastrpc_req_mmap {
	__s32 fd;
	__u32 flags;	/* flags for dsp to map with */
	__u64 vaddrin;	/* optional virtual address */
	__u64 size;	/* size */
	__u64 vaddrout;	/* dsp virtual address */
};

struct fastrpc_mem_map {
	__s32 version;
	__s32 fd;		/* fd */
	__s32 offset;		/* buffer offset */
	__u32 flags;		/* flags defined in enum fastrpc_map_flags */
	__u64 vaddrin;		/* buffer virtual address */
	__u64 length;		/* buffer length */
	__u64 vaddrout;		/* [out] remote virtual address */
	__s32 attrs;		/* buffer attributes used for SMMU mapping */
	__s32 reserved[4];
};

struct fastrpc_req_munmap {
	__u64 vaddrout;	/* address to unmap */
	__u64 size;	/* size */
};

struct fastrpc_mem_unmap {
	__s32 vesion;
	__s32 fd;		/* fd */
	__u64 vaddr;		/* remote process (dsp) virtual address */
	__u64 length;		/* buffer size */
	__u32 attr;		/* Attributes for FD */
	__s32 reserved[4];
};

/* Types of context manage requests */
enum fastrpc_mdctx_manage_req {
	/* Setup multidomain context in kernel */
	FASTRPC_MDCTX_SETUP,
	/* Delete multidomain context in kernel */
	FASTRPC_MDCTX_REMOVE,
};

/* Payload for FASTRPC_INVOKE_MDCTX_MANAGE type */
struct fastrpc_ioctl_mdctx_manage {
	/*
	 * Type of ctx manage request.
	 * One of "enum fastrpc_mdctx_manage_req"
	 */
	__u32 req;
	/* ctx id in userspace */
	__u64 user_ctx;
	/* User-addr to list of domains on which context is being managed */
	__u64 domain_ids;
	/* User-addr to list of domains on which context is being managed */
	__u64 session_ids;
	/* Number of domain ids */
	__u32 num_domains;
	/*
	 * ctx setup req  : user-addr where unique 64-bit context generated
	 *                  by kernel is copied to
	 * ctx remove req : ctx id to be removed
	 */
	__u64 ctx;
	__u32 reserved[FASTRPC_MDCTX_IOCTL_RSVD];
};

/* Payload for FASTRPC_INVOKE_REMOTE_PROCESS_STATE_DUMP type */
struct fastrpc_ioctl_remote_proc_state_dump {
	/* Domain id of dsp on which remote process is running */
	__u32 domain;

	/* Session id of remote process */
	__u32 session;

	/* Level of logging on remote subsystem */
	__u32 level;

	/* String buffer where process state log will be written to */
	__s32 fd;

	/* Size of buffer */
	__u32 size;

	__u32 reserved[5];
};

/* Payload for FASTRPC_INVOKE_SET_RPC_TIMEOUT type */
struct fastrpc_internal_proc_timeout {
	/* Timeout in ms */
	__u32 timeout;
	/* Reserved for future use */
	__u32 reserved[FASTRPC_RPC_TIMEOUT_IOCTL_RSVD];
};

/* Payload for FASTRPC_INVOKE_KERNEL_LOG_RETRIEVE type */
struct fastrpc_ioctl_kernel_log {
	/*
	 * User-space address of buffer where kernel log data
	 * will be copied to
	 */
	__u64 buffer;
	/* Size of the buffer in bytes */
	__u32 buffer_size;
	/* number of bytes copied into 'buffer' */
	__u32 out_copied;
};

/* Thread types used with FASTRPC_INVOKE_THREAD_EXIT */
enum fastrpc_thread_type {
	/* User-space logger thread */
	FASTRPC_THREAD_LOGGER = 1,
};

/* Payload for FASTRPC_INVOKE_THREAD_EXIT */
struct fastrpc_thread_exit {
	/* Thread type that is exiting (logger, worker, etc.) */
	__u32 thread_type;
};

/* Payload for FASTRPC_INVOKE_REMOTE_WORK */
struct fastrpc_ioctl_remote_work {
	/* FastRPC or DSPQueue handle */
	__u64 handle;
	/* Job priority (START only) */
	__u32 priority;
	/* Application or process ID */
	__u32 appid;
	/* remote_job_type */
	__u32 type;
	/* remote_job_status_type START or END */
	__u32 status;
	/* Pointer to inference group_id string */
	__u64 group_id;
	/* Length of group_id string */
	__u32 group_id_len;
	/* Userspace ptr to feature_id str */
	__u64 feature_id;
	/* Length of feature_id string */
	__u32 feature_id_len;
	/* reserved for future use */
	__u32 reserved[8];
};

enum fastrpc_control_type {
	FASTRPC_CONTROL_LATENCY		=	1,
	FASTRPC_CONTROL_SMMU		=	2,
	FASTRPC_CONTROL_KALLOC		=	3,
	FASTRPC_CONTROL_WAKELOCK	=	4,
	FASTRPC_CONTROL_PM		=	5,
	/* Clean process on DSP */
	FASTRPC_CONTROL_DSPPROCESS_CLEAN	=	6,
	FASTRPC_CONTROL_RPC_POLL	=	7,
	FASTRPC_CONTROL_ASYNC_WAKE	=	8,
	FASTRPC_CONTROL_NOTIF_WAKE	=	9,
};

enum fastrpc_dspsignal_type {
	FASTRPC_DSPSIGNAL_SIGNAL = 1,
	FASTRPC_DSPSIGNAL_WAIT = 2,
	FASTRPC_DSPSIGNAL_CREATE = 3,
	FASTRPC_DSPSIGNAL_DESTROY = 4,
	FASTRPC_DSPSIGNAL_CANCEL_WAIT = 5,
};

enum fastrpc_status_flags {
	FASTRPC_USERPD_UP			= 0,
	FASTRPC_USERPD_EXIT			= 1,
	FASTRPC_USERPD_FORCE_KILL	= 2,
	FASTRPC_USERPD_EXCEPTION	= 3,
	FASTRPC_DSP_SSR				= 4,
	FASTRPC_USERPD_RELEASE		= 5,
	FASTRPC_USERPD_TIMEOUT		= 6,
};

struct fastrpc_ioctl_capability {
	__u32 domain;
	__u32 attribute_id;
	__u32 capability;   /* dsp capability */
	__u32 reserved[4];
};

enum fastrpc_perfkeys {
	PERF_COUNT = 0,
	PERF_RESERVED1 = 1,
	PERF_MAP = 2,
	PERF_COPY = 3,
	PERF_LINK = 4,
	PERF_GETARGS = 5,
	PERF_PUTARGS = 6,
	PERF_RESERVED2 = 7,
	PERF_INVOKE = 8,
	PERF_RESERVED3 = 9,
	PERF_KEY_MAX = 10,
};

#endif /* __QCOM_FASTRPC_H__ */
