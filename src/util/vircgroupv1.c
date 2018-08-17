/*
 * vircgroupv1.c: methods for cgroups v1 backend
 *
 * Copyright (C) 2010-2015,2018 Red Hat, Inc.
 * Copyright IBM Corp. 2008
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
#include <config.h>

#if defined HAVE_MNTENT_H && defined HAVE_GETMNTENT_R
# include <mntent.h>
#endif
#include <sys/stat.h>
#if defined HAVE_SYS_MOUNT_H
# include <sys/mount.h>
#endif

#include "internal.h"

#define __VIR_CGROUP_ALLOW_INCLUDE_PRIV_H__
#include "vircgrouppriv.h"
#undef __VIR_CGROUP_ALLOW_INCLUDE_PRIV_H__

#include "vircgroup.h"
#include "vircgroupbackend.h"
#include "vircgroupv1.h"
#include "virfile.h"
#include "virlog.h"
#include "virstring.h"
#include "virsystemd.h"
#include "virerror.h"

VIR_LOG_INIT("util.cgroup");

#define VIR_FROM_THIS VIR_FROM_CGROUP


VIR_ENUM_DECL(virCgroupV1Controller);
VIR_ENUM_IMPL(virCgroupV1Controller, VIR_CGROUP_CONTROLLER_LAST,
              "cpu", "cpuacct", "cpuset", "memory", "devices",
              "freezer", "blkio", "net_cls", "perf_event",
              "name=systemd");


/* We're looking for at least one 'cgroup' fs mount,
 * which is *not* a named mount. */
static bool
virCgroupV1Available(void)
{
    bool ret = false;
    FILE *mounts = NULL;
    struct mntent entry;
    char buf[CGROUP_MAX_VAL];

    if (!virFileExists("/proc/cgroups"))
        return false;

    if (!(mounts = fopen("/proc/mounts", "r")))
        return false;

    while (getmntent_r(mounts, &entry, buf, sizeof(buf)) != NULL) {
        if (STREQ(entry.mnt_type, "cgroup") && !strstr(entry.mnt_opts, "name=")) {
            ret = true;
            break;
        }
    }

    VIR_FORCE_FCLOSE(mounts);
    return ret;
}


static bool
virCgroupV1ValidateMachineGroup(virCgroupPtr group,
                                const char *name,
                                const char *drivername,
                                const char *machinename)
{
    size_t i;
    VIR_AUTOFREE(char *) partname = NULL;
    VIR_AUTOFREE(char *) scopename_old = NULL;
    VIR_AUTOFREE(char *) scopename_new = NULL;
    VIR_AUTOFREE(char *) partmachinename = NULL;

    if (virAsprintf(&partname, "%s.libvirt-%s",
                    name, drivername) < 0)
        return false;

    if (virCgroupPartitionEscape(&partname) < 0)
        return false;

    if (virAsprintf(&partmachinename, "%s.libvirt-%s",
                    machinename, drivername) < 0 ||
        virCgroupPartitionEscape(&partmachinename) < 0)
        return false;

    if (!(scopename_old = virSystemdMakeScopeName(name, drivername, true)))
        return false;

    if (!(scopename_new = virSystemdMakeScopeName(machinename,
                                                  drivername, false)))
        return false;

    if (virCgroupPartitionEscape(&scopename_old) < 0)
        return false;

    if (virCgroupPartitionEscape(&scopename_new) < 0)
        return false;

    for (i = 0; i < VIR_CGROUP_CONTROLLER_LAST; i++) {
        char *tmp;

        if (i == VIR_CGROUP_CONTROLLER_SYSTEMD)
            continue;

        if (!group->controllers[i].placement)
            continue;

        tmp = strrchr(group->controllers[i].placement, '/');
        if (!tmp)
            return false;

        if (i == VIR_CGROUP_CONTROLLER_CPU ||
            i == VIR_CGROUP_CONTROLLER_CPUACCT ||
            i == VIR_CGROUP_CONTROLLER_CPUSET) {
            if (STREQ(tmp, "/emulator"))
                *tmp = '\0';
            tmp = strrchr(group->controllers[i].placement, '/');
            if (!tmp)
                return false;
        }

        tmp++;

        if (STRNEQ(tmp, name) &&
            STRNEQ(tmp, machinename) &&
            STRNEQ(tmp, partname) &&
            STRNEQ(tmp, partmachinename) &&
            STRNEQ(tmp, scopename_old) &&
            STRNEQ(tmp, scopename_new)) {
            VIR_DEBUG("Name '%s' for controller '%s' does not match "
                      "'%s', '%s', '%s', '%s' or '%s'",
                      tmp, virCgroupV1ControllerTypeToString(i),
                      name, machinename, partname,
                      scopename_old, scopename_new);
            return false;
        }
    }

    return true;
}


static int
virCgroupV1CopyMounts(virCgroupPtr group,
                      virCgroupPtr parent)
{
    size_t i;
    for (i = 0; i < VIR_CGROUP_CONTROLLER_LAST; i++) {
        if (!parent->controllers[i].mountPoint)
            continue;

        if (VIR_STRDUP(group->controllers[i].mountPoint,
                       parent->controllers[i].mountPoint) < 0)
            return -1;

        if (VIR_STRDUP(group->controllers[i].linkPoint,
                       parent->controllers[i].linkPoint) < 0)
            return -1;
    }
    return 0;
}


static int
virCgroupV1CopyPlacement(virCgroupPtr group,
                         const char *path,
                         virCgroupPtr parent)
{
    size_t i;
    for (i = 0; i < VIR_CGROUP_CONTROLLER_LAST; i++) {
        if (!group->controllers[i].mountPoint)
            continue;

        if (i == VIR_CGROUP_CONTROLLER_SYSTEMD)
            continue;

        if (path[0] == '/') {
            if (VIR_STRDUP(group->controllers[i].placement, path) < 0)
                return -1;
        } else {
            /*
             * parent == "/" + path="" => "/"
             * parent == "/libvirt.service" + path == "" => "/libvirt.service"
             * parent == "/libvirt.service" + path == "foo" => "/libvirt.service/foo"
             */
            if (virAsprintf(&group->controllers[i].placement,
                            "%s%s%s",
                            parent->controllers[i].placement,
                            (STREQ(parent->controllers[i].placement, "/") ||
                             STREQ(path, "") ? "" : "/"),
                            path) < 0)
                return -1;
        }
    }

    return 0;
}


static int
virCgroupV1ResolveMountLink(const char *mntDir,
                            const char *typeStr,
                            virCgroupControllerPtr controller)
{
    VIR_AUTOFREE(char *) linkSrc = NULL;
    VIR_AUTOFREE(char *) tmp = NULL;
    char *dirName;
    struct stat sb;

    if (VIR_STRDUP(tmp, mntDir) < 0)
        return -1;

    dirName = strrchr(tmp, '/');
    if (!dirName) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Missing '/' separator in cgroup mount '%s'"), tmp);
        return -1;
    }

    if (!strchr(dirName + 1, ','))
        return 0;

    *dirName = '\0';
    if (virAsprintf(&linkSrc, "%s/%s", tmp, typeStr) < 0)
        return -1;
    *dirName = '/';

    if (lstat(linkSrc, &sb) < 0) {
        if (errno == ENOENT) {
            VIR_WARN("Controller %s co-mounted at %s is missing symlink at %s",
                     typeStr, tmp, linkSrc);
        } else {
            virReportSystemError(errno, _("Cannot stat %s"), linkSrc);
            return -1;
        }
    } else {
        if (!S_ISLNK(sb.st_mode)) {
            VIR_WARN("Expecting a symlink at %s for controller %s",
                     linkSrc, typeStr);
        } else {
            VIR_STEAL_PTR(controller->linkPoint, linkSrc);
        }
    }

    return 0;
}


static bool
virCgroupV1MountOptsMatchController(const char *mntOpts,
                                    const char *typeStr)
{
    const char *tmp = mntOpts;
    int typeLen = strlen(typeStr);

    while (tmp) {
        const char *next = strchr(tmp, ',');
        int len;
        if (next) {
            len = next - tmp;
            next++;
        } else {
            len = strlen(tmp);
        }

        if (typeLen == len && STREQLEN(typeStr, tmp, len))
            return true;

        tmp = next;
    }

    return false;
}


static int
virCgroupV1DetectMounts(virCgroupPtr group,
                        const char *mntType,
                        const char *mntOpts,
                        const char *mntDir)
{
    size_t i;

    if (STRNEQ(mntType, "cgroup"))
        return 0;

    for (i = 0; i < VIR_CGROUP_CONTROLLER_LAST; i++) {
        const char *typestr = virCgroupV1ControllerTypeToString(i);

        if (virCgroupV1MountOptsMatchController(mntOpts, typestr)) {
            /* Note that the lines in /proc/mounts have the same
             * order than the mount operations, and that there may
             * be duplicates due to bind mounts. This means
             * that the same mount point may be processed more than
             * once. We need to save the results of the last one,
             * and we need to be careful to release the memory used
             * by previous processing. */
            virCgroupControllerPtr controller = &group->controllers[i];

            VIR_FREE(controller->mountPoint);
            VIR_FREE(controller->linkPoint);
            if (VIR_STRDUP(controller->mountPoint, mntDir) < 0)
                return -1;

            /* If it is a co-mount it has a filename like "cpu,cpuacct"
             * and we must identify the symlink path */
            if (virCgroupV1ResolveMountLink(mntDir, typestr, controller) < 0)
                return -1;
        }
    }

    return 0;
}


static int
virCgroupV1DetectPlacement(virCgroupPtr group,
                           const char *path,
                           const char *controllers,
                           const char *selfpath)
{
    size_t i;

    for (i = 0; i < VIR_CGROUP_CONTROLLER_LAST; i++) {
        const char *typestr = virCgroupV1ControllerTypeToString(i);

        if (virCgroupV1MountOptsMatchController(controllers, typestr) &&
            group->controllers[i].mountPoint != NULL &&
            group->controllers[i].placement == NULL) {
            /*
             * selfpath == "/" + path="" -> "/"
             * selfpath == "/libvirt.service" + path == "" -> "/libvirt.service"
             * selfpath == "/libvirt.service" + path == "foo" -> "/libvirt.service/foo"
             */
            if (i == VIR_CGROUP_CONTROLLER_SYSTEMD) {
                if (VIR_STRDUP(group->controllers[i].placement,
                               selfpath) < 0)
                    return -1;
            } else {
                if (virAsprintf(&group->controllers[i].placement,
                                "%s%s%s", selfpath,
                                (STREQ(selfpath, "/") ||
                                 STREQ(path, "") ? "" : "/"),
                                path) < 0)
                    return -1;
            }
        }
    }

    return 0;
}


static int
virCgroupV1ValidatePlacement(virCgroupPtr group,
                             pid_t pid)
{
    size_t i;

    for (i = 0; i < VIR_CGROUP_CONTROLLER_LAST; i++) {
        if (!group->controllers[i].mountPoint)
            continue;

        if (!group->controllers[i].placement) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not find placement for v1 controller %s at %s"),
                           virCgroupV1ControllerTypeToString(i),
                           group->controllers[i].placement);
            return -1;
        }

        VIR_DEBUG("Detected mount/mapping %zu:%s at %s in %s for pid %lld",
                  i,
                  virCgroupV1ControllerTypeToString(i),
                  group->controllers[i].mountPoint,
                  group->controllers[i].placement,
                  (long long) pid);
    }

    return 0;
}


static char *
virCgroupV1StealPlacement(virCgroupPtr group)
{
    char *ret = NULL;

    VIR_STEAL_PTR(ret, group->controllers[VIR_CGROUP_CONTROLLER_SYSTEMD].placement);

    return ret;
}


static int
virCgroupV1DetectControllers(virCgroupPtr group,
                             int controllers)
{
    size_t i;
    size_t j;

    if (controllers >= 0) {
        VIR_DEBUG("Filtering controllers %d", controllers);
        /* First mark requested but non-existing controllers to be ignored */
        for (i = 0; i < VIR_CGROUP_CONTROLLER_LAST; i++) {
            if (((1 << i) & controllers)) {
                /* Remove non-existent controllers  */
                if (!group->controllers[i].mountPoint) {
                    VIR_DEBUG("Requested controller '%s' not mounted, ignoring",
                              virCgroupV1ControllerTypeToString(i));
                    controllers &= ~(1 << i);
                }
            }
        }
        for (i = 0; i < VIR_CGROUP_CONTROLLER_LAST; i++) {
            VIR_DEBUG("Controller '%s' wanted=%s, mount='%s'",
                      virCgroupV1ControllerTypeToString(i),
                      (1 << i) & controllers ? "yes" : "no",
                      NULLSTR(group->controllers[i].mountPoint));
            if (!((1 << i) & controllers) &&
                group->controllers[i].mountPoint) {
                /* Check whether a request to disable a controller
                 * clashes with co-mounting of controllers */
                for (j = 0; j < VIR_CGROUP_CONTROLLER_LAST; j++) {
                    if (j == i)
                        continue;
                    if (!((1 << j) & controllers))
                        continue;

                    if (STREQ_NULLABLE(group->controllers[i].mountPoint,
                                       group->controllers[j].mountPoint)) {
                        virReportSystemError(EINVAL,
                                             _("V1 controller '%s' is not wanted, but '%s' is co-mounted"),
                                             virCgroupV1ControllerTypeToString(i),
                                             virCgroupV1ControllerTypeToString(j));
                        return -1;
                    }
                }
                VIR_FREE(group->controllers[i].mountPoint);
            }
        }
    } else {
        VIR_DEBUG("Auto-detecting controllers");
        controllers = 0;
        for (i = 0; i < VIR_CGROUP_CONTROLLER_LAST; i++) {
            VIR_DEBUG("Controller '%s' present=%s",
                      virCgroupV1ControllerTypeToString(i),
                      group->controllers[i].mountPoint ? "yes" : "no");
            if (group->controllers[i].mountPoint == NULL)
                continue;
            controllers |= (1 << i);
        }
    }

    return controllers;
}


static bool
virCgroupV1HasController(virCgroupPtr group,
                         int controller)
{
    return group->controllers[controller].mountPoint != NULL;
}


static int
virCgroupV1GetAnyController(virCgroupPtr group)
{
    size_t i;

    for (i = 0; i < VIR_CGROUP_CONTROLLER_LAST; i++) {
        /* Reject any controller with a placement
         * of '/' to avoid doing bad stuff to the root
         * cgroup
         */
        if (group->controllers[i].mountPoint &&
            group->controllers[i].placement &&
            STRNEQ(group->controllers[i].placement, "/")) {
            return i;
        }
    }

    return -1;
}


static int
virCgroupV1PathOfController(virCgroupPtr group,
                            int controller,
                            const char *key,
                            char **path)
{
    if (group->controllers[controller].mountPoint == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("v1 controller '%s' is not mounted"),
                       virCgroupV1ControllerTypeToString(controller));
        return -1;
    }

    if (group->controllers[controller].placement == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("v1 controller '%s' is not enabled for group"),
                       virCgroupV1ControllerTypeToString(controller));
        return -1;
    }

    if (virAsprintf(path, "%s%s/%s",
                    group->controllers[controller].mountPoint,
                    group->controllers[controller].placement,
                    key ? key : "") < 0)
        return -1;

    return 0;
}


static int
virCgroupV1CpuSetInherit(virCgroupPtr parent,
                         virCgroupPtr group)
{
    size_t i;
    const char *inherit_values[] = {
        "cpuset.cpus",
        "cpuset.mems",
        "cpuset.memory_migrate",
    };

    VIR_DEBUG("Setting up inheritance %s -> %s", parent->path, group->path);
    for (i = 0; i < ARRAY_CARDINALITY(inherit_values); i++) {
        VIR_AUTOFREE(char *) value = NULL;

        if (virCgroupGetValueStr(parent,
                                 VIR_CGROUP_CONTROLLER_CPUSET,
                                 inherit_values[i],
                                 &value) < 0)
            return -1;

        VIR_DEBUG("Inherit %s = %s", inherit_values[i], value);

        if (virCgroupSetValueStr(group,
                                 VIR_CGROUP_CONTROLLER_CPUSET,
                                 inherit_values[i],
                                 value) < 0)
            return -1;
    }

    return 0;
}


static int
virCgroupV1SetMemoryUseHierarchy(virCgroupPtr group)
{
    unsigned long long value;
    const char *filename = "memory.use_hierarchy";

    if (virCgroupGetValueU64(group,
                             VIR_CGROUP_CONTROLLER_MEMORY,
                             filename, &value) < 0)
        return -1;

    /* Setting twice causes error, so if already enabled, skip setting */
    if (value == 1)
        return 0;

    VIR_DEBUG("Setting up %s/%s", group->path, filename);
    if (virCgroupSetValueU64(group,
                             VIR_CGROUP_CONTROLLER_MEMORY,
                             filename, 1) < 0)
        return -1;

    return 0;
}


static int
virCgroupV1MakeGroup(virCgroupPtr parent,
                     virCgroupPtr group,
                     bool create,
                     unsigned int flags)
{
    size_t i;

    VIR_DEBUG("Make group %s", group->path);
    for (i = 0; i < VIR_CGROUP_CONTROLLER_LAST; i++) {
        VIR_AUTOFREE(char *) path = NULL;

        /* We must never mkdir() in systemd's hierarchy */
        if (i == VIR_CGROUP_CONTROLLER_SYSTEMD) {
            VIR_DEBUG("Not creating systemd controller group");
            continue;
        }

        /* Skip over controllers that aren't mounted */
        if (!group->controllers[i].mountPoint) {
            VIR_DEBUG("Skipping unmounted controller %s",
                      virCgroupV1ControllerTypeToString(i));
            continue;
        }

        if (virCgroupV1PathOfController(group, i, "", &path) < 0)
            return -1;

        VIR_DEBUG("Make controller %s", path);
        if (!virFileExists(path)) {
            if (!create ||
                mkdir(path, 0755) < 0) {
                if (errno == EEXIST)
                    continue;
                /* With a kernel that doesn't support multi-level directory
                 * for blkio controller, libvirt will fail and disable all
                 * other controllers even though they are available. So
                 * treat blkio as unmounted if mkdir fails. */
                if (i == VIR_CGROUP_CONTROLLER_BLKIO) {
                    VIR_DEBUG("Ignoring mkdir failure with blkio controller. Kernel probably too old");
                    VIR_FREE(group->controllers[i].mountPoint);
                    continue;
                } else {
                    virReportSystemError(errno,
                                         _("Failed to create v1 controller %s for group"),
                                         virCgroupV1ControllerTypeToString(i));
                    return -1;
                }
            }
            if (i == VIR_CGROUP_CONTROLLER_CPUSET &&
                group->controllers[i].mountPoint != NULL &&
                virCgroupV1CpuSetInherit(parent, group) < 0) {
                return -1;
            }
            /*
             * Note that virCgroupV1SetMemoryUseHierarchy should always be
             * called prior to creating subcgroups and attaching tasks.
             */
            if ((flags & VIR_CGROUP_MEM_HIERACHY) &&
                i == VIR_CGROUP_CONTROLLER_MEMORY &&
                group->controllers[i].mountPoint != NULL &&
                virCgroupV1SetMemoryUseHierarchy(group) < 0) {
                return -1;
            }
        }
    }

    VIR_DEBUG("Done making controllers for group");
    return 0;
}


static int
virCgroupV1Remove(virCgroupPtr group)
{
    int rc = 0;
    size_t i;

    VIR_DEBUG("Removing cgroup %s", group->path);
    for (i = 0; i < VIR_CGROUP_CONTROLLER_LAST; i++) {
        VIR_AUTOFREE(char *) grppath = NULL;

        /* Skip over controllers not mounted */
        if (!group->controllers[i].mountPoint)
            continue;

        /* We must never rmdir() in systemd's hierarchy */
        if (i == VIR_CGROUP_CONTROLLER_SYSTEMD)
            continue;

        /* Don't delete the root group, if we accidentally
           ended up in it for some reason */
        if (STREQ(group->controllers[i].placement, "/"))
            continue;

        if (virCgroupV1PathOfController(group,
                                        i,
                                        NULL,
                                        &grppath) != 0)
            continue;

        VIR_DEBUG("Removing cgroup %s and all child cgroups", grppath);
        rc = virCgroupRemoveRecursively(grppath);
    }
    VIR_DEBUG("Done removing cgroup %s", group->path);

    return rc;
}


static int
virCgroupV1AddTask(virCgroupPtr group,
                   pid_t pid,
                   unsigned int flags)
{
    int ret = -1;
    size_t i;

    for (i = 0; i < VIR_CGROUP_CONTROLLER_LAST; i++) {
        /* Skip over controllers not mounted */
        if (!group->controllers[i].mountPoint)
            continue;

        /* We must never add tasks in systemd's hierarchy
         * unless we're intentionally trying to move a
         * task into a systemd machine scope */
        if (i == VIR_CGROUP_CONTROLLER_SYSTEMD &&
            !(flags & VIR_CGROUP_TASK_SYSTEMD))
            continue;

        if (virCgroupSetValueI64(group, i, "tasks", pid) < 0)
            goto cleanup;
    }

    ret = 0;
 cleanup:
    return ret;
}


static int
virCgroupV1HasEmptyTasks(virCgroupPtr cgroup,
                         int controller)
{
    int ret = -1;
    VIR_AUTOFREE(char *) content = NULL;

    if (!cgroup)
        return -1;

    ret = virCgroupGetValueStr(cgroup, controller, "tasks", &content);

    if (ret == 0 && content[0] == '\0')
        ret = 1;

    return ret;
}


static char *
virCgroupV1IdentifyRoot(virCgroupPtr group)
{
    char *ret = NULL;
    size_t i;

    for (i = 0; i < VIR_CGROUP_CONTROLLER_LAST; i++) {
        char *tmp;
        if (!group->controllers[i].mountPoint)
            continue;
        if (!(tmp = strrchr(group->controllers[i].mountPoint, '/'))) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not find directory separator in %s"),
                           group->controllers[i].mountPoint);
            return NULL;
        }

        if (VIR_STRNDUP(ret, group->controllers[i].mountPoint,
                        tmp - group->controllers[i].mountPoint) < 0)
            return NULL;
        return ret;
    }

    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                   _("Could not find any mounted v1 controllers"));
    return NULL;
}


static int
virCgroupV1BindMount(virCgroupPtr group,
                     const char *oldroot,
                     const char *mountopts)
{
    size_t i;
    VIR_AUTOFREE(char *) opts = NULL;
    VIR_AUTOFREE(char *) root = NULL;

    if (!(root = virCgroupV1IdentifyRoot(group)))
        return -1;

    VIR_DEBUG("Mounting cgroups at '%s'", root);

    if (virFileMakePath(root) < 0) {
        virReportSystemError(errno,
                             _("Unable to create directory %s"),
                             root);
        return -1;
    }

    if (virAsprintf(&opts,
                    "mode=755,size=65536%s", mountopts) < 0)
        return -1;

    if (mount("tmpfs", root, "tmpfs", MS_NOSUID|MS_NODEV|MS_NOEXEC, opts) < 0) {
        virReportSystemError(errno,
                             _("Failed to mount %s on %s type %s"),
                             "tmpfs", root, "tmpfs");
        return -1;
    }

    for (i = 0; i < VIR_CGROUP_CONTROLLER_LAST; i++) {
        if (!group->controllers[i].mountPoint)
            continue;

        if (!virFileExists(group->controllers[i].mountPoint)) {
            VIR_AUTOFREE(char *) src = NULL;
            if (virAsprintf(&src, "%s%s",
                            oldroot,
                            group->controllers[i].mountPoint) < 0)
                return -1;

            VIR_DEBUG("Create mount point '%s'",
                      group->controllers[i].mountPoint);
            if (virFileMakePath(group->controllers[i].mountPoint) < 0) {
                virReportSystemError(errno,
                                     _("Unable to create directory %s"),
                                     group->controllers[i].mountPoint);
                return -1;
            }

            if (mount(src, group->controllers[i].mountPoint, "none", MS_BIND,
                      NULL) < 0) {
                virReportSystemError(errno,
                                     _("Failed to bind cgroup '%s' on '%s'"),
                                     src, group->controllers[i].mountPoint);
                return -1;
            }
        }

        if (group->controllers[i].linkPoint) {
            VIR_DEBUG("Link mount point '%s' to '%s'",
                      group->controllers[i].mountPoint,
                      group->controllers[i].linkPoint);
            if (symlink(group->controllers[i].mountPoint,
                        group->controllers[i].linkPoint) < 0) {
                virReportSystemError(errno,
                                     _("Unable to symlink directory %s to %s"),
                                     group->controllers[i].mountPoint,
                                     group->controllers[i].linkPoint);
                return -1;
            }
        }
    }

    return 0;
}


static int
virCgroupV1SetOwner(virCgroupPtr cgroup,
                    uid_t uid,
                    gid_t gid,
                    int controllers)
{
    int ret = -1;
    size_t i;
    DIR *dh = NULL;
    int direrr;

    for (i = 0; i < VIR_CGROUP_CONTROLLER_LAST; i++) {
        VIR_AUTOFREE(char *) base = NULL;
        struct dirent *de;

        if (!((1 << i) & controllers))
            continue;

        if (!cgroup->controllers[i].mountPoint)
            continue;

        if (virAsprintf(&base, "%s%s", cgroup->controllers[i].mountPoint,
                        cgroup->controllers[i].placement) < 0)
            goto cleanup;

        if (virDirOpen(&dh, base) < 0)
            goto cleanup;

        while ((direrr = virDirRead(dh, &de, base)) > 0) {
            VIR_AUTOFREE(char *) entry = NULL;

            if (virAsprintf(&entry, "%s/%s", base, de->d_name) < 0)
                goto cleanup;

            if (chown(entry, uid, gid) < 0) {
                virReportSystemError(errno,
                                     _("cannot chown '%s' to (%u, %u)"),
                                     entry, uid, gid);
                goto cleanup;
            }
        }
        if (direrr < 0)
            goto cleanup;

        if (chown(base, uid, gid) < 0) {
            virReportSystemError(errno,
                                 _("cannot chown '%s' to (%u, %u)"),
                                 base, uid, gid);
            goto cleanup;
        }

        VIR_DIR_CLOSE(dh);
    }

    ret = 0;

 cleanup:
    VIR_DIR_CLOSE(dh);
    return ret;
}


static int
virCgroupV1SetBlkioWeight(virCgroupPtr group,
                          unsigned int weight)
{
    return virCgroupSetValueU64(group,
                                VIR_CGROUP_CONTROLLER_BLKIO,
                                "blkio.weight",
                                weight);
}


static int
virCgroupV1GetBlkioWeight(virCgroupPtr group,
                          unsigned int *weight)
{
    unsigned long long tmp;
    int ret;
    ret = virCgroupGetValueU64(group,
                               VIR_CGROUP_CONTROLLER_BLKIO,
                               "blkio.weight", &tmp);
    if (ret == 0)
        *weight = tmp;
    return ret;
}


virCgroupBackend virCgroupV1Backend = {
    .type = VIR_CGROUP_BACKEND_TYPE_V1,

    .available = virCgroupV1Available,
    .validateMachineGroup = virCgroupV1ValidateMachineGroup,
    .copyMounts = virCgroupV1CopyMounts,
    .copyPlacement = virCgroupV1CopyPlacement,
    .detectMounts = virCgroupV1DetectMounts,
    .detectPlacement = virCgroupV1DetectPlacement,
    .validatePlacement = virCgroupV1ValidatePlacement,
    .stealPlacement = virCgroupV1StealPlacement,
    .detectControllers = virCgroupV1DetectControllers,
    .hasController = virCgroupV1HasController,
    .getAnyController = virCgroupV1GetAnyController,
    .pathOfController = virCgroupV1PathOfController,
    .makeGroup = virCgroupV1MakeGroup,
    .remove = virCgroupV1Remove,
    .addTask = virCgroupV1AddTask,
    .hasEmptyTasks = virCgroupV1HasEmptyTasks,
    .bindMount = virCgroupV1BindMount,
    .setOwner = virCgroupV1SetOwner,

    .setBlkioWeight = virCgroupV1SetBlkioWeight,
    .getBlkioWeight = virCgroupV1GetBlkioWeight,
};


void
virCgroupV1Register(void)
{
    virCgroupBackendRegister(&virCgroupV1Backend);
}