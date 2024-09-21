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

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "qemu/module.h"
#include "sysemu/reset.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/qdev-properties.h"
#include "hw/misc/vmfwupdate.h"
#include "qemu/error-report.h"
#include "sysemu/kvm.h"
#include "sysemu/runstate.h"


static uint8_t get_vmfwupdate_plat(void)
{
    MachineState *machine;
    MachineClass *mc;
    Object *m_obj = qdev_get_machine();

    if (object_dynamic_cast(m_obj, TYPE_MACHINE)) {
        machine = MACHINE(m_obj);
        mc = MACHINE_GET_CLASS(machine);
        return mc->vmfwupdate_plat;
    }

    return 0x0;
}

static void fw_update_reset(void *dev)
{
    VMFwUpdateState *s = VMFWUPDATE(dev);

    s->has_fw_blob = false;
    //s->n_entries = 0;
    s->fw_cfg_ctl = 0;
    //memset(&s->vmfwupdate_blobs, 0, sizeof(s->vmfwupdate_blobs));
    memset(&s->cpu_state, 0, sizeof(s->cpu_state));

printf("XXX %s:%d\n", __func__, __LINE__);
}

#if 0
#include "hw/i386/pc.h"
static FWCfgState* get_x86_fw_cfg(void) {
    MachineState *machine;
    X86MachineState *x86ms;
    Object *m_obj = qdev_get_machine();

    if (object_dynamic_cast(m_obj, TYPE_MACHINE)) {
        machine = MACHINE(m_obj);
        x86ms = X86_MACHINE(machine);
        return x86ms->fw_cfg;
    }

    return NULL;
}
#endif

#include "qemu/log.h"

static void regenerate_sev_vm(VMFwUpdateState *s) {
    MachineState *ms;

    Object *m_obj = qdev_get_machine();
    if (!object_dynamic_cast(m_obj, TYPE_MACHINE)) { /* is this check needed? */
        return;
    }
    ms = MACHINE(m_obj);

    qemu_loglevel |= CPU_LOG_TB_IN_ASM | CPU_LOG_TB_CPU;

    if (ms->cgs) {
        /* mark guest state as mutable so that we can initiate a reset */
        kvm_mark_guest_state_mutable();
    }

    /*
     * initiate reset.
     * TODO: do we really need the special flag SHUTDOWN_CAUSE_SEV_RESET
     * or do we allow all confidential resets to regenerate sev context
     * upon reset?
     */
    qemu_system_reset_request(SHUTDOWN_CAUSE_SEV_RESET);

    return;
}

static void fw_ctrl_write(void *dev, off_t offset, size_t len) {
    VMFwUpdateState *s = VMFWUPDATE(dev);
    FwCfgVmFwUpdateBlob *entry;
    bool *blob_found;
    int i;
    FWCfgState *fw_cfg = fw_cfg_find(); // or should we use get_x86_fw_cfg()?

    if (!s->enabled) {
        return;
    }

    if (!s->has_fw_blob)
        return;

    if (!fw_cfg) {
        return; /* should not happen */
    }

    blob_found = g_malloc0(VMFW_TYPE_BLOB_MAX * sizeof(bool));

    /* do some validations */
    for (i = 0; i < s->n_entries; i++) {
        entry = &s->vmfwupdate_blobs[i];
        if ((entry->blob_type == 0) || (entry->blob_type > VMFW_TYPE_BLOB_MAX)) {
            warn_report("vmfwupdate: incorrect type of blob passed!");
            goto failed;
        }

        if (blob_found[entry->blob_type]) {
            warn_report("vmfwupdate: multiple blobs of the same type not allowed!");
            goto failed;
        }

        if ((entry->map_type != VMFW_TYPE_MAP_PRIVATE) &&
            (entry->map_type != VMFW_TYPE_MAP_SHARED)) {
            warn_report("vmfwupdate: incorrect map type specified!");
            goto failed;
        }
        blob_found[entry->blob_type] = true;
    } /* end of validations */

    g_free(blob_found);

    /*
     * Set the EDK fw_cfg files to appropriate values here. See x86_load_linux()
     * EDK linux loader will read these fwcfg selector values and act on it.
     */
    if (0) {
    for (i = 0; i < s->n_entries; i++) {
        entry = &s->vmfwupdate_blobs[i];
        switch (entry->blob_type) {
        case VMFW_TYPE_BLOB_KERNEL:
            fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_ADDR, entry->paddr);
            fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_SIZE, entry->size);
            break;
        case VMFW_TYPE_BLOB_INITRD:
            fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_ADDR, entry->paddr);
            fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_SIZE, entry->size);
            break;
        case VMFW_TYPE_BLOB_CMDLINE:
            fw_cfg_add_i32(fw_cfg, FW_CFG_CMDLINE_ADDR, entry->paddr);
            fw_cfg_add_i32(fw_cfg, FW_CFG_CMDLINE_SIZE, entry->size);
            break;
        case VMFW_TYPE_BLOB_FW:
            // TODO  - copy the firmware just below 4G guest pmem?
            break;
        case VMFW_TYPE_BLOB_SETUP:
            // Anything to do here? Probably not.
            break;
        default:
            /* should not reach here */
            warn_report("vmfwupdate: Invalid blob. Unable to trigger vm regeration "
                    "with provided launch digests!");
            return;
        }
    }
    }

    /*
     * TODO: Also register the guest provided cpu states somewhere so that upon
     * reset, the guest initializes CPU registers to those values.
     * Not required if we use the standard locations for firmware?
     */

    switch ((char) s->fw_cfg_ctl) {
    case 't':
        /*
         * trigger reboot of the guest with known state and blobs in the
         * specified memory location.
         */
        regenerate_sev_vm(s);
        /* does not return */
        break;
    case 'd':
        /* kill switch - disable update mechanism */
        s->enabled = false;
        break;
    default:
        warn_report("vmfwupdate: option not recognized!");
    }

    return;

 failed:
    g_free(blob_found);
    warn_report("vmfwupdate: validations have failed. Unable to trigger vm regeration "
                "with provided launch digests!");
    s->has_fw_blob = false;
    return;
}

static void fw_cpustate_write(void *dev, off_t offset, size_t len) {
    VMFwUpdateState *s = VMFWUPDATE(dev);

    if (!s->enabled) {
        return;
    }
    return;
}

static void fw_blob_write(void *dev, off_t offset, size_t len) {
    VMFwUpdateState *s = VMFWUPDATE(dev);
    FwCfgVmFwUpdateBlob *entry;
    int n_entries;

    if (!s->enabled) {
        warn_report("vmfwupdate: device not enabled!");
        return;
    }
    /*
     * This function is supposed to do some basic validations on the
     * blobs the guest has written. If the basic validations pass, the
     * function sets a flag that indicates that a valid blob written by the
     * guest is present.
     */
    if (len % sizeof(*entry)) {
        /* incorrect size of buffer written, bail out */
        warn_report("vmfwupdate: incorrect size of blob entries passed!");
        return;
    }

    n_entries = len / sizeof (*entry);

    if (n_entries > MAX_VMFWUPD_ENTRIES) {
        /* incorrect number of entries, bail out */
        warn_report("vmfwupdate: incorrect number of entries passed by guest!");
        return;
    }

    s->has_fw_blob = (offset == 0 &&
                      len <= sizeof(s->vmfwupdate_blobs));
    if (!s->has_fw_blob)
        return;

    s->n_entries = n_entries;

}

static void vmfwupdate_realize(DeviceState *dev, Error **errp)
{
    VMFwUpdateState *s = VMFWUPDATE(dev);
    FWCfgState *fw_cfg = fw_cfg_find();

    /* multiple devices are not supported */
    if (!vmfwupdate_find()) {
        error_setg(errp, "at most one %s device is permitted",
                   TYPE_VMFWUPDATE);
        return;
    }

    if (!fw_cfg) {
        error_setg(errp, "%s device requires fw_cfg",
                   TYPE_VMFWUPDATE);
        return;
    }

    fw_cfg_add_file_callback(fw_cfg, FILE_VMFWUPDATE_BLOB,
                             NULL, fw_blob_write, s,
                             &s->vmfwupdate_blobs,
                             sizeof(s->vmfwupdate_blobs),
                             false);

    /*
     * Add global capability and platform for fw_cfg file. This will be used
     * to determine if the binary blobs (kernel, firmware etc) in the guest
     * is compatible with the underlying host platform (hypervisor).
     */
    s->platcap = cpu_to_le16((VMFWUPDPLATMSK & get_vmfwupdate_plat()) |
                             CAP_VMFWUPD_MASK | CAP_EDKROM_MASK);
    fw_cfg_add_file(fw_cfg, FILE_VMFWUPDATE_CAP, &s->platcap, sizeof(s->platcap));

    /* add callback for collecting initial cpu state data */
    fw_cfg_add_file_callback(fw_cfg, FILE_VMFWUPDATE_CPUSTATE,
                             NULL, fw_cpustate_write, s,
                             &s->cpu_state,
                             sizeof(s->cpu_state),
                             false);
    /*
     * add fw cfg control file to trigger the vm regenration with the launch
     * blobs. This is because, we do not want to trigger the re-launch when
     * the blobs are still not completely written.
     */
    fw_cfg_add_file_callback(fw_cfg, FILE_VMFWUPDATE_CONTROL,
                             NULL, fw_ctrl_write, s,
                             &s->fw_cfg_ctl,
                             sizeof(s->fw_cfg_ctl),
                             false);
    /*
     * This device requires to register a global reset because it is
     * not plugged to a bus (which, as its QOM parent, would reset it).
     */
    qemu_register_reset(fw_update_reset, dev);
}

static Property vmfwupdate_properties[] = {
    /*
     * the device can also be disabled by writing 'd' to fw_cfg file
     * '/etc/fwupdate-control'
     */
    DEFINE_PROP_BOOL("enabled", VMFwUpdateState, enabled, true),
    DEFINE_PROP_END_OF_LIST(),
};

static void vmfwupdate_device_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    /* we are not interested in migration - so no need to populate dc->vmsd */
    dc->desc = "VM boot blob update device";
    dc->realize = vmfwupdate_realize;
    dc->hotpluggable = false;
    device_class_set_props(dc, vmfwupdate_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo vmfwupdate_device_info = {
    .name          = TYPE_VMFWUPDATE,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(VMFwUpdateState),
    .class_init    = vmfwupdate_device_class_init,
};

static void vmfwupdate_register_types(void)
{
    type_register_static(&vmfwupdate_device_info);
}

type_init(vmfwupdate_register_types)
