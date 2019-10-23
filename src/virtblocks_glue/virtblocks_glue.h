/*
 * virtblocks_glue.h: Glue for Virt Blocks code
 *
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "virtblocks.h"
#include "conf/domain_conf.h"

int virDomainConvertToVirtBlocks(virDomainDef *from,
                                 VirtBlocksVmDescription **to);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VirtBlocksError,
                              virtblocks_error_free);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VirtBlocksArray,
                              virtblocks_array_free);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VirtBlocksVmDescription,
                              virtblocks_vm_description_free);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VirtBlocksVmDisk,
                              virtblocks_vm_disk_free);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VirtBlocksVmSerial,
                              virtblocks_vm_serial_free);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VirtBlocksVmMemballoon,
                              virtblocks_vm_memballoon_free);
