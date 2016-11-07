/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2007-2010 David Zeuthen <zeuthen@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"
#include <glib/gi18n-lib.h>

#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/file.h>

#include <glib/gstdio.h>

#include <blockdev/fs.h>
#include <blockdev/part.h>

#include "udiskslogging.h"
#include "udiskslinuxpartitiontable.h"
#include "udiskslinuxblockobject.h"
#include "udisksdaemon.h"
#include "udisksdaemonutil.h"
#include "udiskslinuxdevice.h"
#include "udiskslinuxblock.h"

/**
 * SECTION:udiskslinuxpartitiontable
 * @title: UDisksLinuxPartitionTable
 * @short_description: Linux implementation of #UDisksPartitionTable
 *
 * This type provides an implementation of the #UDisksPartitionTable
 * interface on Linux.
 */

typedef struct _UDisksLinuxPartitionTableClass   UDisksLinuxPartitionTableClass;

/**
 * UDisksLinuxPartitionTable:
 *
 * The #UDisksLinuxPartitionTable structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _UDisksLinuxPartitionTable
{
  UDisksPartitionTableSkeleton parent_instance;
};

struct _UDisksLinuxPartitionTableClass
{
  UDisksPartitionTableSkeletonClass parent_class;
};

static void partition_table_iface_init (UDisksPartitionTableIface *iface);

G_DEFINE_TYPE_WITH_CODE (UDisksLinuxPartitionTable, udisks_linux_partition_table, UDISKS_TYPE_PARTITION_TABLE_SKELETON,
                         G_IMPLEMENT_INTERFACE (UDISKS_TYPE_PARTITION_TABLE, partition_table_iface_init));

/* ---------------------------------------------------------------------------------------------------- */

static void
udisks_linux_partition_table_init (UDisksLinuxPartitionTable *partition_table)
{
  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (partition_table),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
}

static void
udisks_linux_partition_table_class_init (UDisksLinuxPartitionTableClass *klass)
{
}

/**
 * udisks_linux_partition_table_new:
 *
 * Creates a new #UDisksLinuxPartitionTable instance.
 *
 * Returns: A new #UDisksLinuxPartitionTable. Free with g_object_unref().
 */
UDisksPartitionTable *
udisks_linux_partition_table_new (void)
{
  return UDISKS_PARTITION_TABLE (g_object_new (UDISKS_TYPE_LINUX_PARTITION_TABLE,
                                               NULL));
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_linux_partition_table_update:
 * @table: A #UDisksLinuxPartitionTable.
 * @object: The enclosing #UDisksLinuxBlockObject instance.
 *
 * Updates the interface.
 */
void
udisks_linux_partition_table_update (UDisksLinuxPartitionTable  *table,
                                     UDisksLinuxBlockObject     *object)
{
  const gchar *type = NULL;
  UDisksLinuxDevice *device = NULL;;

  device = udisks_linux_block_object_get_device (object);
  if (device != NULL)
    type = g_udev_device_get_property (device->udev_device, "ID_PART_TABLE_TYPE");

  udisks_partition_table_set_type_ (UDISKS_PARTITION_TABLE (table), type);

  g_clear_object (&device);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
ranges_overlap (guint64 a_offset, guint64 a_size,
                guint64 b_offset, guint64 b_size)
{
  guint64 a1 = a_offset, a2 = a_offset + a_size;
  guint64 b1 = b_offset, b2 = b_offset + b_size;
  gboolean ret = FALSE;

  /* There are only two cases when these intervals can overlap
   *
   * 1.  [a1-------a2]
   *               [b1------b2]
   *
   * 2.            [a1-------a2]
   *     [b1------b2]
   */

  if (a1 <= b1)
    {
      /* case 1 */
      if (a2 > b1)
        {
          ret = TRUE;
          goto out;
        }
    }
  else
    {
      /* case 2 */
      if (b2 > a1)
        {
          ret = TRUE;
          goto out;
        }
    }

 out:
  return ret;
}

static gboolean
have_partition_in_range (UDisksPartitionTable *table,
                         UDisksObject         *object,
                         guint64               start,
                         guint64               end,
                         gboolean              ignore_container)
{
  gboolean ret = FALSE;
  UDisksDaemon *daemon = NULL;
  GDBusObjectManager *object_manager = NULL;
  const gchar *table_object_path;
  GList *objects = NULL, *l;

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  object_manager = G_DBUS_OBJECT_MANAGER (udisks_daemon_get_object_manager (daemon));

  table_object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (object));

  objects = g_dbus_object_manager_get_objects (object_manager);
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *i_object = UDISKS_OBJECT (l->data);
      UDisksPartition *i_partition = NULL;

      i_partition = udisks_object_get_partition (i_object);

      if (i_partition == NULL)
        goto cont;

      if (g_strcmp0 (udisks_partition_get_table (i_partition), table_object_path) != 0)
        goto cont;

      if (ignore_container && udisks_partition_get_is_container (i_partition))
        goto cont;

      if (!ranges_overlap (start, end - start,
                           udisks_partition_get_offset (i_partition), udisks_partition_get_size (i_partition)))
        goto cont;

      ret = TRUE;
      g_clear_object (&i_partition);
      goto out;

    cont:
      g_clear_object (&i_partition);
    }

 out:
  g_list_foreach (objects, (GFunc) g_object_unref, NULL);
  g_list_free (objects);
  return ret;
}

static UDisksPartition *
find_container_partition (UDisksPartitionTable *table,
                          UDisksObject         *object,
                          guint64               start,
                          guint64               end)
{
  UDisksPartition *ret = NULL;
  UDisksDaemon *daemon = NULL;
  GDBusObjectManager *object_manager = NULL;
  const gchar *table_object_path;
  GList *objects = NULL, *l;

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  object_manager = G_DBUS_OBJECT_MANAGER (udisks_daemon_get_object_manager (daemon));

  table_object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (object));

  objects = g_dbus_object_manager_get_objects (object_manager);
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *i_object = UDISKS_OBJECT (l->data);
      UDisksPartition *i_partition = NULL;

      i_partition = udisks_object_get_partition (i_object);

      if (i_partition == NULL)
        goto cont;

      if (g_strcmp0 (udisks_partition_get_table (i_partition), table_object_path) != 0)
        goto cont;

      if (udisks_partition_get_is_container (i_partition)
          && ranges_overlap (start, end - start,
                             udisks_partition_get_offset (i_partition),
                             udisks_partition_get_size (i_partition)))
        {
          ret = i_partition;
          goto out;
        }

    cont:
      g_clear_object (&i_partition);
    }

 out:
  g_list_foreach (objects, (GFunc) g_object_unref, NULL);
  g_list_free (objects);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  UDisksObject *partition_table_object;
  guint64       pos_to_wait_for;
  gboolean      ignore_container;
} WaitForPartitionData;

static UDisksObject *
wait_for_partition (UDisksDaemon *daemon,
                    gpointer      user_data)
{
  WaitForPartitionData *data = user_data;
  UDisksObject *ret = NULL;
  GList *objects, *l;

  objects = udisks_daemon_get_objects (daemon);
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksPartition *partition = udisks_object_get_partition (object);
      if (partition != NULL)
        {
          if (g_strcmp0 (udisks_partition_get_table (partition),
                         g_dbus_object_get_object_path (G_DBUS_OBJECT (data->partition_table_object))) == 0)
            {
              guint64 offset = udisks_partition_get_offset (partition);
              guint64 size = udisks_partition_get_size (partition);

              if (data->pos_to_wait_for >= offset && data->pos_to_wait_for < offset + size)
                {
                  if (!(udisks_partition_get_is_container (partition) && data->ignore_container))
                    {
                      g_object_unref (partition);
                      ret = g_object_ref (object);
                      goto out;
                    }
                }
            }
          g_object_unref (partition);
        }
    }

 out:
  g_list_free_full (objects, g_object_unref);
  return ret;
}

#define MIB_SIZE (1048576L)

static UDisksObject *
udisks_linux_partition_table_handle_create_partition (UDisksPartitionTable   *table,
                                                      GDBusMethodInvocation  *invocation,
                                                      guint64                 offset,
                                                      guint64                 size,
                                                      const gchar            *type,
                                                      const gchar            *name,
                                                      GVariant               *options)
{
  const gchar *action_id = NULL;
  const gchar *message = NULL;
  UDisksBlock *block = NULL;
  UDisksObject *object = NULL;
  UDisksDaemon *daemon = NULL;
  gchar *device_name = NULL;
  WaitForPartitionData *wait_data = NULL;
  UDisksObject *partition_object = NULL;
  UDisksBlock *partition_block = NULL;
  BDPartSpec *part_spec = NULL;
  const gchar *table_type;
  uid_t caller_uid;
  gid_t caller_gid;
  gboolean do_wipe = TRUE;
  GError *error = NULL;

  object = udisks_daemon_util_dup_object (table, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  block = udisks_object_get_block (object);
  if (block == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Partition table object is not a block device");
      goto out;
    }

  error = NULL;
  if (!udisks_daemon_util_get_caller_uid_sync (daemon,
                                               invocation,
                                               NULL /* GCancellable */,
                                               &caller_uid,
                                               &caller_gid,
                                               NULL,
                                               &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
      goto out;
    }

  action_id = "org.freedesktop.udisks2.modify-device";
  /* Translators: Shown in authentication dialog when the user
   * requests creating a new partition.
   *
   * Do not translate $(drive), it's a placeholder and
   * will be replaced by the name of the drive/device in question
   */
  message = N_("Authentication is required to create a partition on $(drive)");
  if (!udisks_daemon_util_setup_by_user (daemon, object, caller_uid))
    {
      if (udisks_block_get_hint_system (block))
        {
          action_id = "org.freedesktop.udisks2.modify-device-system";
        }
      else if (!udisks_daemon_util_on_user_seat (daemon, object, caller_uid))
        {
          action_id = "org.freedesktop.udisks2.modify-device-other-seat";
        }
    }

  if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                    object,
                                                    action_id,
                                                    options,
                                                    message,
                                                    invocation))
    goto out;

  device_name = g_strdup (udisks_block_get_device (block));

  table_type = udisks_partition_table_get_type_ (table);
  wait_data = g_new0 (WaitForPartitionData, 1);
  if (g_strcmp0 (table_type, "dos") == 0)
    {
      guint64 start_mib;
      guint64 end_bytes;
      guint64 max_end_bytes;
      BDPartTypeReq part_type;
      char *endp;
      gint type_as_int;
      gboolean is_logical = FALSE;

      max_end_bytes = udisks_block_get_size (block);

      if (strlen (name) > 0)
        {
          g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                                 "MBR partition table does not support names");
          goto out;
        }

      /* Determine whether we are creating a primary, extended or logical partition */
      type_as_int = strtol (type, &endp, 0);
      if (type[0] != '\0' && *endp == '\0' &&
          (type_as_int == 0x05 || type_as_int == 0x0f || type_as_int == 0x85))
        {
          part_type = BD_PART_TYPE_REQ_EXTENDED;
          do_wipe = FALSE;  // wiping an extended partition destroys it
          if (have_partition_in_range (table, object, offset, offset + size, FALSE))
            {
              g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                                     "Requested range is already occupied by a partition");
              goto out;
            }
        }
      else
        {
          if (have_partition_in_range (table, object, offset, offset + size, FALSE))
            {
              if (have_partition_in_range (table, object, offset, offset + size, TRUE))
                {
                  g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                                         "Requested range is already occupied by a partition");
                  goto out;
                }
              else
                {
                  UDisksPartition *container = find_container_partition (table, object,
                                                                         offset, offset + size);
                  g_assert (container != NULL);
                  is_logical = TRUE;
                  part_type = BD_PART_TYPE_REQ_LOGICAL;
                  max_end_bytes = (udisks_partition_get_offset(container)
                                   + udisks_partition_get_size(container));
                }
            }
          else
            {
              part_type = BD_PART_TYPE_REQ_NORMAL;
            }
        }

      /* Ensure we _start_ at MiB granularity since that ensures optimal IO...
       * Also round up size to nearest multiple of 512
       */
      start_mib = offset / MIB_SIZE + 1L;
      end_bytes = start_mib * MIB_SIZE + ((size + 511L) & (~511L));

      /* Now reduce size until we are not
       *
       *  - overlapping neighboring partitions; or
       *  - exceeding the end of the disk
       */
      while (end_bytes > start_mib * MIB_SIZE && (have_partition_in_range (table,
                                                                           object,
                                                                           start_mib * MIB_SIZE,
                                                                           end_bytes, is_logical) ||
                                                  end_bytes > max_end_bytes))
        {
          /* TODO: if end_bytes is sufficiently big this could be *a lot* of loop iterations
           *       and thus a potential DoS attack...
           */
          end_bytes -= 512L;
        }
      wait_data->pos_to_wait_for = (start_mib*MIB_SIZE + end_bytes) / 2L;
      wait_data->ignore_container = is_logical;

      size = end_bytes - (start_mib * MIB_SIZE); /* recalculate size with the new end_bytes */
      if (! bd_part_create_part (device_name, part_type, start_mib * MIB_SIZE,
                                 size, BD_PART_ALIGN_OPTIMAL, &error))
        {
          g_dbus_method_invocation_take_error (invocation, error);
          goto out;
        }

    }
  else if (g_strcmp0 (table_type, "gpt") == 0)
    {
      guint64 start_mib;
      guint64 end_bytes;

      /* GPT is easy, no extended/logical crap */
      if (have_partition_in_range (table, object, offset, offset + size, FALSE))
        {
          g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                                 "Requested range is already occupied by a partition");
          goto out;
        }

      /* Ensure we _start_ at MiB granularity since that ensures optimal IO...
       * Also round up size to nearest multiple of 512
       */
      start_mib = offset / MIB_SIZE + 1L;
      end_bytes = start_mib * MIB_SIZE + ((size + 511L) & (~511L));

      /* Now reduce size until we are not
       *
       *  - overlapping neighboring partitions; or
       *  - exceeding the end of the disk (note: the 33 LBAs is the Secondary GPT)
       */
      while (end_bytes > start_mib * MIB_SIZE && (have_partition_in_range (table,
                                                                           object,
                                                                           start_mib * MIB_SIZE,
                                                                           end_bytes, FALSE) ||
                                                  (end_bytes > udisks_block_get_size (block) - 33*512)))
        {
          /* TODO: if end_bytes is sufficiently big this could be *a lot* of loop iterations
           *       and thus a potential DoS attack...
           */
          end_bytes -= 512L;
        }

      size = end_bytes - (start_mib * MIB_SIZE); /* recalculate size with the new end_bytes */
      wait_data->pos_to_wait_for = (start_mib*MIB_SIZE + end_bytes) / 2L;
      part_spec = bd_part_create_part (device_name, BD_PART_TYPE_REQ_NORMAL, start_mib * MIB_SIZE,
                                       size, BD_PART_ALIGN_OPTIMAL, &error);
      if (part_spec == NULL)
        {
          g_dbus_method_invocation_take_error (invocation, error);
          goto out;
        }

      /* set name if given */
      if (strlen (name) > 0)
        {
          gchar *part_name = g_strdup (name);
          if (! bd_part_set_part_name (device_name, part_spec->path, part_name, &error))
            {
              g_dbus_method_invocation_take_error (invocation, error);
              goto out;
            }
        }

    }
  else
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Don't know how to create partitions this partition table of type `%s'",
                                             table_type);
      goto out;
    }

  /* this is sometimes needed because parted(8) does not generate the uevent itself */
  udisks_linux_block_object_trigger_uevent (UDISKS_LINUX_BLOCK_OBJECT (object));

  /* sit and wait for the partition to show up */
  g_warn_if_fail (wait_data->pos_to_wait_for > 0);
  wait_data->partition_table_object = object;
  error = NULL;
  partition_object = udisks_daemon_wait_for_object_sync (daemon,
                                                         wait_for_partition,
                                                         wait_data,
                                                         NULL,
                                                         30,
                                                         &error);
  if (partition_object == NULL)
    {
      g_prefix_error (&error, "Error waiting for partition to appear: ");
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }
  partition_block = udisks_object_get_block (partition_object);
  if (partition_block == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Partition object is not a block device");
      g_clear_object (&partition_object);
      goto out;
    }

  /* TODO: set partition type */

  /* wipe the newly created partition if wanted */
  if (do_wipe)
    {
      if (! bd_fs_wipe (part_spec->path, TRUE, &error))
        {
          g_prefix_error (&error, "Error wiping newly created partition: ");
          g_dbus_method_invocation_take_error (invocation, error);
          g_clear_object (&partition_object);
          goto out;
        }
    }

  /* this is sometimes needed because parted(8) does not generate the uevent itself */
  udisks_linux_block_object_trigger_uevent (UDISKS_LINUX_BLOCK_OBJECT (partition_object));

 out:
  g_free (wait_data);
  g_clear_object (&partition_block);
  g_free (device_name);
  g_clear_object (&object);
  g_clear_object (&block);
  return partition_object;
}

static int
flock_block_dev (UDisksPartitionTable *iface)
{
  UDisksObject *object = udisks_daemon_util_dup_object (iface, NULL);
  UDisksBlock *block = object? udisks_object_peek_block (object) : NULL;
  int fd = block? open (udisks_block_get_device (block), O_RDONLY) : -1;

  if (fd >= 0)
    flock (fd, LOCK_SH | LOCK_NB);

  g_clear_object (&object);
  return fd;
}

static void
unflock_block_dev (int fd)
{
  if (fd >= 0)
    close (fd);
}

/* runs in thread dedicated to handling @invocation */
static gboolean
handle_create_partition (UDisksPartitionTable   *table,
                         GDBusMethodInvocation  *invocation,
                         guint64                 offset,
                         guint64                 size,
                         const gchar            *type,
                         const gchar            *name,
                         GVariant               *options)
{
  /* We (try to) take a shared lock on the partition table while
     creating and formatting a new partition, here and also in
     handle_create_partition_and_format.

     This lock prevents udevd from issuing a BLKRRPART ioctl call.
     That ioctl is undesired because it might temporarily remove the
     block device of the newly created block device.  It does so only
     temporarily, but it still happens that the block device is
     missing exactly when wipefs or mkfs try to access it.

     Also, a pair of remove/add events will cause storaged to create a
     new internal UDisksObject to represent the block device of the
     partition.  The code currently doesn't handle this and waits for
     changes (such as an expected filesystem type or UUID) to a
     obsolete internal object that will never see them.
  */

  int fd = flock_block_dev (table);
  UDisksObject *partition_object =
    udisks_linux_partition_table_handle_create_partition (table,
                                                          invocation,
                                                          offset,
                                                          size,
                                                          type,
                                                          name,
                                                          options);

  if (partition_object)
    {
      udisks_partition_table_complete_create_partition
        (table, invocation, g_dbus_object_get_object_path (G_DBUS_OBJECT (partition_object)));
      g_object_unref (partition_object);
    }

  unflock_block_dev (fd);

  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* runs in thread dedicated to handling @invocation */
struct FormatCompleteData {
  UDisksPartitionTable *table;
  GDBusMethodInvocation *invocation;
  UDisksObject *partition_object;
  int lock_fd;
};

static void
handle_format_complete (gpointer user_data)
{
  struct FormatCompleteData *data = user_data;
  udisks_partition_table_complete_create_partition
    (data->table, data->invocation, g_dbus_object_get_object_path (G_DBUS_OBJECT (data->partition_object)));
  unflock_block_dev (data->lock_fd);
}

static gboolean
handle_create_partition_and_format (UDisksPartitionTable   *table,
                                    GDBusMethodInvocation  *invocation,
                                    guint64                 offset,
                                    guint64                 size,
                                    const gchar            *type,
                                    const gchar            *name,
                                    GVariant               *options,
                                    const gchar            *format_type,
                                    GVariant               *format_options)
{
  /* See handle_create_partition for a motivation of taking the lock.
   */

  int fd = flock_block_dev (table);
  UDisksObject *partition_object =
    udisks_linux_partition_table_handle_create_partition (table,
                                                          invocation,
                                                          offset,
                                                          size,
                                                          type,
                                                          name,
                                                          options);

  if (partition_object)
    {
      struct FormatCompleteData data;
      data.table = table;
      data.invocation = invocation;
      data.partition_object = partition_object;
      data.lock_fd = fd;
      udisks_linux_block_handle_format (udisks_object_peek_block (partition_object),
                                        invocation,
                                        format_type,
                                        format_options,
                                        handle_format_complete, &data);
      g_object_unref (partition_object);
    }
  else
    unflock_block_dev (fd);

  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static void
partition_table_iface_init (UDisksPartitionTableIface *iface)
{
  iface->handle_create_partition = handle_create_partition;
  iface->handle_create_partition_and_format = handle_create_partition_and_format;
}
