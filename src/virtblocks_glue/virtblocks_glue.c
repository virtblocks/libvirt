#include <config.h>

#include "util/virerror.h"
#include "virlog.h"

#include "virtblocks_glue.h"

#define VIR_FROM_THIS VIR_FROM_VIRTBLOCKS

VIR_LOG_INIT("virtblocks");

static int
virDomainConvertToVirtBlocksSerial(virDomainDef *from,
                                   VirtBlocksVmSerial **to)
{
    g_autoptr(VirtBlocksVmSerial) serial = NULL;
    virDomainChrDef *fromSerial = NULL;

    if (from->nserials != 1)
        return -1;

    fromSerial = from->serials[0];

    if (fromSerial->source->type != VIR_DOMAIN_CHR_TYPE_UNIX ||
        fromSerial->source->data.nix.listen != true ||
        fromSerial->targetModel != VIR_DOMAIN_CHR_SERIAL_TARGET_MODEL_ISA_SERIAL) {
        return -1;
    }

    serial = virtblocks_vm_serial_new();
    virtblocks_vm_serial_set_path(serial,
                                  fromSerial->source->data.nix.path);

    *to = g_steal_pointer(&serial);

    return 0;
}

static int
virDomainConvertToVirtBlocksDisk(virDomainDef *from,
                                 VirtBlocksVmDisk **to)
{
    g_autoptr(VirtBlocksVmDisk) disk = NULL;
    virDomainDiskDef *fromDisk = NULL;

    if (from->ndisks != 1)
        return -1;

    fromDisk = from->disks[0];

    if (fromDisk->device != VIR_DOMAIN_DISK_DEVICE_DISK ||
        fromDisk->bus != VIR_DOMAIN_DISK_BUS_VIRTIO ||
        virDomainDiskGetType(fromDisk) != VIR_STORAGE_TYPE_FILE ||
        virDomainDiskGetFormat(fromDisk) != VIR_STORAGE_FILE_QCOW2 ||
        STRNEQ(virDomainDiskGetDriver(fromDisk), "qemu")) {
        return -1;
    }

    disk = virtblocks_vm_disk_new();
    virtblocks_vm_disk_set_filename(disk,
                                    virDomainDiskGetSource(fromDisk));

    *to = g_steal_pointer(&disk);

    return 0;
}

int
virDomainConvertToVirtBlocks(virDomainDef *from,
                             VirtBlocksVmDescription **to)
{
    g_autoptr(VirtBlocksVmDescription) vm = NULL;
    g_autoptr(VirtBlocksVmDisk) disk = NULL;
    g_autoptr(VirtBlocksVmSerial) serial = NULL;
    VirtBlocksVmModel model;

    if (STRPREFIX(from->os.machine, "pc")) {
        model = VIRTBLOCKS_VM_MODEL_LEGACY_V1;
    } else if (STRPREFIX(from->os.machine, "q35")) {
        model = VIRTBLOCKS_VM_MODEL_MODERN_V1;
    } else {
        goto error;
    }

    vm = virtblocks_vm_description_new(model);

    if (virDomainConvertToVirtBlocksDisk(from, &disk) < 0 ||
        virDomainConvertToVirtBlocksSerial(from, &serial) < 0) {
        goto error;
    }

    virtblocks_vm_description_set_emulator(vm, from->emulator);
    virtblocks_vm_description_set_disk_slots(vm, 1);
    virtblocks_vm_description_set_disk_for_slot(vm, disk, 0);
    virtblocks_vm_description_set_serial(vm, serial);

    /* These only work for the simplest of cases */
    virtblocks_vm_description_set_cpus(vm, virDomainDefGetVcpusMax(from));
    virtblocks_vm_description_set_memory(vm, virDomainDefGetMemoryInitial(from) / 1024);

    *to = g_steal_pointer(&vm);

    return 0;

 error:
    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                   "%s",
                   "Unsupported configuration");
    return -1;
}