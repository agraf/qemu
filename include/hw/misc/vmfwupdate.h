/*
 * Virtual Machine firmware update device
 *
 * Copyright (C) 2024 Red Hat, Inc.
 *
 * Authors: Ani Sinha <anisinha@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
#ifndef VMFWUPDATE_H
#define VMFWUPDATE_H

#include "hw/qdev-core.h"
#include "qemu/units.h"
#include "qemu/osdep.h"
#include "qom/object.h"
#include "qemu/event_notifier.h"
#include "linux-headers/asm-x86/kvm.h"

#define TYPE_VMFWUPDATE "vmfwupdate"

#define VMFWUPDPLATMSK 0x07 /* last three bits are platform bits */
#define VMFWUPDCAPMSK  ~VMFWUPDPLATMSK /* most significant 13 capability bits */

#define VMFWUPDATE_PC_PLATFORM 0x01
#define CAP_EDKROM_MASK 0x08 /* bit 4 represents support for EDKROM */
/*
 * bit 15 of the compatibility bits indicates to EDK that kernel/initrd etc
 * blobs are present in the guest memory already at the address specified
 * through fw_cfg files FW_CFG_KERNEL_ADDR, FW_CFG_INITRD_ADDR etc. So no
 * need to read the kernel/initrd data separately into guest memory. This way
 * there is no memory copy required from guest to host and again back from host
 * to guest for guest initialization measurement blobs. EDK reads the file
 * FILE_VMFWUPDATE_CAP and checks the MSB. If the file is absent or if the bit
 * is not set, it takes the conventional path. If the file is present and the
 * bit is set, it takes the new path. edk2 must be made aware of
 * FILE_VMFWUPDATE_CAP and CAP_VMFWUPD_MASK and CAP_EDKROM_MASK and also
 * VMFWUPDATE_PC_PLATFORM for checking platform its running on.
 */
#define CAP_VMFWUPD_MASK 0x80

#define MAX_FW_BLOB_SIZE 16 * MiB
#define MAX_VMFWUPD_ENTRIES 256

/* fw_cfg file definitions */
#define FILE_VMFWUPDATE_BLOB "etc/vmfwupdate-blob"
#define FILE_VMFWUPDATE_CPUSTATE "etc/vmfwupdate-cpu"
#define FILE_VMFWUPDATE_CAP "etc/fwupdate-cap"
#define FILE_VMFWUPDATE_CONTROL "etc/fwupdate-control"

typedef struct VMFwUpdateState VMFwUpdateState;
typedef struct FwCfgVmFwUpdate FwCfgVmFwUpdate;

OBJECT_DECLARE_SIMPLE_TYPE(VMFwUpdateState, VMFWUPDATE);

/* type of mapping requested */
#define VMFW_TYPE_MAP_PRIVATE 0x00
#define VMFW_TYPE_MAP_SHARED 0x01

typedef enum {
    VMFW_TYPE_BLOB_KERNEL = 0x01, /* kernel */
    VMFW_TYPE_BLOB_SETUP, /* need to check if this is required */
    VMFW_TYPE_BLOB_INITRD, /* initrd */
    VMFW_TYPE_BLOB_CMDLINE, /* command line */
    VMFW_TYPE_BLOB_FW, /* firmware */
    VMFW_TYPE_BLOB_MAX
} blob_type_t;

/*
 * The guest sets these values and passes it to the host/hypervisor. This
 * is how the old unsecured VM context passes data to the new secured VM
 * context without use of the control plane. The data resides in the VM
 * memory from where the new context can find them and make use of it.
 * There is no memory remapping, so the paddr of the blob remains where it is
 * specified.
 */
typedef struct FwCfgVmFwUpdateBlob {
    /*
     * blob_type indicates the type of blob/launch digest the guest has passed
     * to the host. blob_type 0x00 is invalid. It is of type blob_type_t.
     */
    uint8_t blob_type;
    /*
     * map_type: type of guest memory mapping requested. Mappings can be either
     * private or shared. Private guest pages are flipped from shared to private
     * when a new SEV guest context is created. The private memory contains CPU
     * state information and firmware blob. The shared memory remains shared
     * with the hypervisor and is excluded from encryption and measurements.
     * The shared data is the next stage artifacts (kernel image/UKI, initrd,
     * command line) that are validated by the second stage firmware present in
     * the private memory. Thus they need not be explicitly measured by ASP.
     */
    uint8_t map_type;
    uint32_t size; /* size of the blob */
    uint64_t paddr; /* starting gpa where the blob is in guest memory. We may
                     * copy the contents of the guest private memory to a
                     * different addresss from paddr
                     */
    uint64_t target_paddr; /* guest physical address where private blobs are
                            * copied to.
                            * XXX: Is this really required to be passed from
                            * the guest?
                            */
} FwCfgVmFwUpdateBlob;

typedef struct FwCfgVmFwUpdateCpuState {
    struct kvm_regs regs;
    /*
     * we are currently building this device only for x86.
     * So using sregs2 is fine even if its only available on x86.
     */
    struct kvm_sregs2 s;
} FwCfgVmFwUpdateCpuState;

struct VMFwUpdateState {
    DeviceState parent_obj;

    bool enabled; /*
                   * the feature can be disabled using a device specific option
                   * or a machine specific compatibilituy flag
                   */
    bool has_fw_blob; /*
                       * true if all the host side validation checks out,
                       * else false
                       */
    /*
     * platform and capabilities
     * least significant 3 bits - platform bits,
     * most significant 13 bits are capability bits.
     * Little endian format.
     */
    uint16_t platcap;

    /*
     * vmgenid fw_cfg ctl
     *   - 't' - trigger vm regeneration. Uses KVM ioctls to regenerate the VM
     *           context.
     */
    uint8_t fw_cfg_ctl;

    /* number of blob entries passed by the guest */
    uint8_t n_entries;

    /*
     * Guest measurement blobs or launch digests - can be firmware blob,
     * kernel blob etc. Number of such blobs is stored in n_entries above.
     */
    FwCfgVmFwUpdateBlob vmfwupdate_blobs[MAX_VMFWUPD_ENTRIES];

    /* Initial guest primary CPU state - we may not require it if we use the
     * "standard" memory location for firmware. Then we may use the default
     * CPU reset vectors.
     * Need to check what happens when IGVM is used instead of EDK2.
     * We do not care about the secondary vcpu states for now.
     */
    FwCfgVmFwUpdateCpuState cpu_state;
};

/* returns NULL unless there is exactly one device */
static inline VMFwUpdateState *vmfwupdate_find(void)
{
    Object *o = object_resolve_path_type("", TYPE_VMFWUPDATE, NULL);

    return o ? VMFWUPDATE(o) : NULL;
}

#endif
