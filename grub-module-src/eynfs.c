/*
 * GRUB EYNFS filesystem module (read-only).
 *
 * This module allows GRUB to locate and read files from EYNFS volumes so
 * kernel + config can live on native EYNFS partitions.
 */

#include <grub/dl.h>
#include <grub/disk.h>
#include <grub/err.h>
#include <grub/file.h>
#include <grub/fs.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/types.h>

GRUB_MOD_LICENSE("GPLv3+");

#define EYNFS_MAGIC 0x45594E46u
#define EYNFS_BLOCK_SIZE 512u
#define EYNFS_NAME_MAX 32u
#define EYNFS_TYPE_FILE 1u
#define EYNFS_TYPE_DIR 2u

/* Legacy full-disk placement fallback (non-partitioned images). */
#define EYNFS_LEGACY_SUPERBLOCK_LBA 2048u

struct eynfs_superblock
{
  grub_uint32_t magic;
  grub_uint32_t version;
  grub_uint32_t block_size;
  grub_uint32_t total_blocks;
  grub_uint32_t root_dir_block;
  grub_uint32_t free_block_map;
  grub_uint32_t name_table_block;
  grub_uint32_t reserved0;
  grub_uint32_t reserved1;
} GRUB_PACKED;

struct eynfs_dir_entry
{
  grub_uint8_t name[EYNFS_NAME_MAX];
  grub_uint8_t type;
  grub_uint8_t flags;
  grub_uint16_t reserved;
  grub_uint32_t size;
  grub_uint32_t first_block;
  grub_uint32_t extra0;
  grub_uint32_t extra1;
} GRUB_PACKED;

struct eynfs_data
{
  struct eynfs_superblock sb;
  grub_uint64_t sb_lba;
  grub_uint32_t file_first_block;
};

static int
eynfs_name_eq (const grub_uint8_t raw_name[EYNFS_NAME_MAX], const char *name)
{
  grub_size_t i;

  for (i = 0; i < EYNFS_NAME_MAX; ++i)
    {
      char c = (char) raw_name[i];
      if (!name[i] && c == '\0')
        return 1;
      if (!name[i] && c != '\0')
        return 0;
      if (c == '\0')
        return name[i] == '\0';
      if ((char) name[i] != c)
        return 0;
    }

  return name[i] == '\0';
}

static void
eynfs_name_copy (char *dst, grub_size_t dst_cap,
                 const grub_uint8_t raw_name[EYNFS_NAME_MAX])
{
  grub_size_t i = 0;

  if (!dst || dst_cap == 0)
    return;

  while (i + 1 < dst_cap && i < EYNFS_NAME_MAX && raw_name[i] != '\0')
    {
      dst[i] = (char) raw_name[i];
      i++;
    }
  dst[i] = '\0';
}

static grub_uint64_t
eynfs_fs_block_to_lba (const struct eynfs_data *d, grub_uint32_t fs_block)
{
  return d->sb_lba + (grub_uint64_t) fs_block;
}

static grub_err_t
eynfs_read_sector (grub_disk_t disk, grub_uint64_t lba, void *out)
{
  return grub_disk_read (disk, lba, 0, EYNFS_BLOCK_SIZE, out);
}

static grub_err_t
eynfs_load_superblock (grub_disk_t disk, struct eynfs_data *d)
{
  grub_err_t err;

  grub_memset (&d->sb, 0, sizeof (d->sb));
  d->sb_lba = 0;

  err = eynfs_read_sector (disk, 0, &d->sb);
  if (err == GRUB_ERR_NONE &&
      d->sb.magic == EYNFS_MAGIC &&
      d->sb.block_size == EYNFS_BLOCK_SIZE)
    {
      d->sb_lba = 0;
      return GRUB_ERR_NONE;
    }

  grub_errno = GRUB_ERR_NONE;
  grub_memset (&d->sb, 0, sizeof (d->sb));
  err = eynfs_read_sector (disk, EYNFS_LEGACY_SUPERBLOCK_LBA, &d->sb);
  if (err)
    return err;

  if (d->sb.magic != EYNFS_MAGIC || d->sb.block_size != EYNFS_BLOCK_SIZE)
    return grub_error (GRUB_ERR_BAD_FS, "not an EYNFS volume");

  d->sb_lba = EYNFS_LEGACY_SUPERBLOCK_LBA;
  return GRUB_ERR_NONE;
}

static grub_err_t
eynfs_find_in_dir (grub_disk_t disk, const struct eynfs_data *d,
                   grub_uint32_t dir_block, const char *name,
                   struct eynfs_dir_entry *out)
{
  grub_uint8_t sector[EYNFS_BLOCK_SIZE];
  grub_uint32_t block = dir_block;

  while (block != 0)
    {
      grub_uint32_t next_block;
      grub_size_t off;

      if (eynfs_read_sector (disk, eynfs_fs_block_to_lba (d, block), sector) != GRUB_ERR_NONE)
        return grub_errno;

      next_block = *(const grub_uint32_t *) (const void *) sector;

      for (off = 4; off + sizeof (struct eynfs_dir_entry) <= EYNFS_BLOCK_SIZE;
           off += sizeof (struct eynfs_dir_entry))
        {
          const struct eynfs_dir_entry *entry;
          if (grub_errno)
            return grub_errno;

          entry = (const struct eynfs_dir_entry *) (const void *) (sector + off);
          if (entry->name[0] == '\0')
            continue;
          if (eynfs_name_eq (entry->name, name))
            {
              if (out)
                *out = *entry;
              return GRUB_ERR_NONE;
            }
        }

      block = next_block;
    }

  return grub_error (GRUB_ERR_FILE_NOT_FOUND, "entry not found");
}

static grub_err_t
eynfs_traverse_path (grub_disk_t disk, const struct eynfs_data *d,
                     const char *path, struct eynfs_dir_entry *out)
{
  char comp[128];
  grub_size_t p = 0;
  grub_uint32_t cur_dir = d->sb.root_dir_block;
  struct eynfs_dir_entry ent;

  if (!path || !out)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "invalid argument");

  while (path[p] == '/')
    p++;

  if (path[p] == '\0')
    {
      grub_memset (&ent, 0, sizeof (ent));
      ent.type = EYNFS_TYPE_DIR;
      ent.first_block = d->sb.root_dir_block;
      *out = ent;
      return GRUB_ERR_NONE;
    }

  while (1)
    {
      grub_size_t n = 0;

      while (path[p] == '/')
        p++;
      while (path[p] != '\0' && path[p] != '/' && n + 1 < sizeof (comp))
        comp[n++] = path[p++];
      comp[n] = '\0';

      if (n == 0)
        return grub_error (GRUB_ERR_BAD_FILENAME, "empty path component");

      if (eynfs_find_in_dir (disk, d, cur_dir, comp, &ent) != GRUB_ERR_NONE)
        return grub_errno;

      while (path[p] == '/')
        p++;
      if (path[p] == '\0')
        {
          *out = ent;
          return GRUB_ERR_NONE;
        }

      if (ent.type != EYNFS_TYPE_DIR)
        return grub_error (GRUB_ERR_BAD_FILE_TYPE, "path component is not a directory");

      cur_dir = ent.first_block;
    }
}

static grub_err_t
eynfs_dir (grub_device_t device, const char *path,
           grub_fs_dir_hook_t hook, void *hook_data)
{
  struct eynfs_data d;
  struct eynfs_dir_entry dir_ent;
  grub_uint8_t sector[EYNFS_BLOCK_SIZE];
  grub_uint32_t block;
  grub_err_t err;

  if (!device || !device->disk)
    return grub_error (GRUB_ERR_BAD_DEVICE, "no disk");

  err = eynfs_load_superblock (device->disk, &d);
  if (err)
    return err;

  err = eynfs_traverse_path (device->disk, &d, path ? path : "/", &dir_ent);
  if (err)
    return err;

  if (dir_ent.type != EYNFS_TYPE_DIR)
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, "not a directory");

  block = dir_ent.first_block;
  while (block != 0)
    {
      grub_uint32_t next_block;
      grub_size_t off;

      err = eynfs_read_sector (device->disk, eynfs_fs_block_to_lba (&d, block), sector);
      if (err)
        return err;

      next_block = *(const grub_uint32_t *) (const void *) sector;

      for (off = 4; off + sizeof (struct eynfs_dir_entry) <= EYNFS_BLOCK_SIZE;
           off += sizeof (struct eynfs_dir_entry))
        {
          const struct eynfs_dir_entry *entry;
          char name[EYNFS_NAME_MAX + 1];
          struct grub_dirhook_info info;

          entry = (const struct eynfs_dir_entry *) (const void *) (sector + off);
          if (entry->name[0] == '\0')
            continue;

          grub_memset (&info, 0, sizeof (info));
          info.dir = (entry->type == EYNFS_TYPE_DIR);
          info.mtime = 0;

          eynfs_name_copy (name, sizeof (name), entry->name);
          if (hook (name, &info, hook_data))
            return GRUB_ERR_NONE;
        }

      block = next_block;
    }

  return GRUB_ERR_NONE;
}

static grub_err_t
eynfs_open (grub_file_t file, const char *name)
{
  struct eynfs_data *d;
  struct eynfs_dir_entry ent;
  grub_err_t err;

  if (!file || !file->device || !file->device->disk)
    return grub_error (GRUB_ERR_BAD_DEVICE, "no disk");

  d = grub_zalloc (sizeof (*d));
  if (!d)
    return grub_errno;

  err = eynfs_load_superblock (file->device->disk, d);
  if (err)
    goto fail;

  err = eynfs_traverse_path (file->device->disk, d, name, &ent);
  if (err)
    goto fail;

  if (ent.type != EYNFS_TYPE_FILE)
    {
      err = grub_error (GRUB_ERR_BAD_FILE_TYPE, "not a regular file");
      goto fail;
    }

  d->file_first_block = ent.first_block;
  file->size = ent.size;
  file->data = d;
  return GRUB_ERR_NONE;

fail:
  grub_free (d);
  return err;
}

static grub_ssize_t
eynfs_read (grub_file_t file, char *buf, grub_size_t len)
{
  struct eynfs_data *d;
  grub_uint8_t sector[EYNFS_BLOCK_SIZE];
  grub_uint32_t block;
  grub_uint64_t remaining;
  grub_uint64_t start;
  grub_size_t copied = 0;

  if (!file || !file->device || !file->device->disk || !buf)
    return -1;

  d = (struct eynfs_data *) file->data;
  if (!d)
    return -1;

  if (file->offset >= file->size)
    return 0;

  if (len > (grub_size_t) (file->size - file->offset))
    len = (grub_size_t) (file->size - file->offset);

  block = d->file_first_block;
  start = file->offset;

  /* Skip full data payload blocks (508 bytes each). */
  while (block != 0 && start >= (EYNFS_BLOCK_SIZE - 4))
    {
      if (eynfs_read_sector (file->device->disk, eynfs_fs_block_to_lba (d, block), sector) != GRUB_ERR_NONE)
        return -1;
      block = *(const grub_uint32_t *) (const void *) sector;
      start -= (EYNFS_BLOCK_SIZE - 4);
    }

  remaining = len;
  while (block != 0 && remaining > 0)
    {
      grub_size_t avail;
      grub_size_t take;

      if (eynfs_read_sector (file->device->disk, eynfs_fs_block_to_lba (d, block), sector) != GRUB_ERR_NONE)
        return -1;

      avail = (grub_size_t) ((EYNFS_BLOCK_SIZE - 4) - start);
      take = (remaining < avail) ? (grub_size_t) remaining : avail;

      grub_memcpy (buf + copied, sector + 4 + start, take);
      copied += take;
      remaining -= take;

      block = *(const grub_uint32_t *) (const void *) sector;
      start = 0;
    }

  return (grub_ssize_t) copied;
}

static grub_err_t
eynfs_close (grub_file_t file)
{
  if (file && file->data)
    {
      grub_free (file->data);
      file->data = NULL;
    }
  return GRUB_ERR_NONE;
}

static struct grub_fs grub_eynfs_fs =
{
  .name = "eynfs",
  .dir = eynfs_dir,
  .open = eynfs_open,
  .read = eynfs_read,
  .close = eynfs_close,
  .next = 0
};

GRUB_MOD_INIT (eynfs)
{
  grub_fs_register (&grub_eynfs_fs);
}

GRUB_MOD_FINI (eynfs)
{
  grub_fs_unregister (&grub_eynfs_fs);
}
