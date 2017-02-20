/*-*- Mode: C; c-basic-offset: 2; indent-tabs-mode: t -*-*/

/* bls.c - implementation of the boot loader spec */

/*
 *  GRUB  --  GRand Unified Bootloader
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/types.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/err.h>
#include <grub/dl.h>
#include <grub/extcmd.h>
#include <grub/i18n.h>
#include <grub/fs.h>
#include <grub/env.h>
#include <grub/file.h>
#include <grub/normal.h>
#include <grub/legacy_parse.h>

GRUB_MOD_LICENSE ("GPLv3+");

#if defined(GRUB_MACHINE_EFI) && !defined(__i386__)
#define GRUB_LINUX_CMD "linuxefi"
#define GRUB_INITRD_CMD "initrdefi"
#else
#define GRUB_LINUX_CMD "linux"
#define GRUB_INITRD_CMD "initrd"
#endif

#define GRUB_BLS_CONFIG_PATH "/boot/loader/entries/"
#define GRUB_APPENDED_INITRAMFS_PATH "/boot/"
#define GRUB_APPENDED_INITRAMFS_FILE "initramfs-append"
#define GRUB_BOOT_DEVICE "($root)/boot"

#define MAIN_ENTRY_TITLE "Endless OS"
#define SUBMENU_TITLE "Advanced ..."

struct boot_entry {
  char *src;
  char *title;
};

#define MAX_ENTRIES 5
struct parse_entry_ctx {
  struct boot_entry boot_entries[MAX_ENTRIES];
  int initramfs_append;
};

static int parse_entry (
    const char *filename,
    const struct grub_dirhook_info *info __attribute__ ((unused)),
    void *data)
{
  grub_size_t n;
  char *p;
  grub_file_t f = NULL;
  grub_off_t sz;
  char *title = NULL, *options = NULL, *clinux = NULL, *initrd = NULL, *root_prepend = NULL;
  const char *root_prepend_env = NULL;
  const char *kparams_env = NULL;
  const char *boot_title = NULL;
  struct parse_entry_ctx *ctx = data;
  struct boot_entry *entry = ctx->boot_entries;

  if (filename[0] == '.')
    return 0;

  n = grub_strlen (filename);
  if (n <= 5)
    return 0;

  if (grub_strcmp (filename + n - 5, ".conf") != 0)
    return 0;

  p = grub_xasprintf (GRUB_BLS_CONFIG_PATH "%s", filename);

  f = grub_file_open (p);
  if (!f)
    goto finish;

  sz = grub_file_size (f);
  if (sz == GRUB_FILE_SIZE_UNKNOWN || sz > 1024*1024)
    goto finish;

  root_prepend_env = grub_env_get ("rootuuid");
  if (root_prepend_env)
    root_prepend = grub_xasprintf ("root=UUID=%s", root_prepend_env);

  kparams_env = grub_env_get ("kparams");

  for (;;)
    {
      char *buf;

      buf = grub_file_getline (f);
      if (!buf)
	break;

      if (grub_strncmp (buf, "title ", 6) == 0)
	{
	  grub_free (title);
	  title = grub_strdup (buf + 6);
	  if (!title)
	    goto finish;
	}
      else if (grub_strncmp (buf, "options ", 8) == 0)
	{
	  grub_free (options);
	  options = grub_strdup (buf + 8);
	  if (!options)
	    goto finish;
	}
      else if (grub_strncmp (buf, "linux ", 6) == 0)
	{
	  grub_free (clinux);
	  clinux = grub_strdup (buf + 6);
	  if (!clinux)
	    goto finish;
	}
      else if (grub_strncmp (buf, "initrd ", 7) == 0)
	{
	  grub_free (initrd);
	  initrd = grub_strdup (buf + 7);
	  if (!initrd)
	    goto finish;
	}

      grub_free(buf);
    }

  if (!linux)
    {
      grub_printf ("Skipping file %s with no 'linux' key.", p);
      goto finish;
    }

  boot_title = title ? title : filename;

  /* find the empty slot */
  while ((entry < (struct boot_entry *) data + MAX_ENTRIES) && entry->src)
    entry++;

  /* no space */
  if (entry == (struct boot_entry *) data + MAX_ENTRIES)
    goto finish;

  /* Escape the title */
  entry->title = grub_legacy_escape(boot_title, grub_strlen (boot_title));

  /* Generate the entry */
  entry->src = grub_xasprintf ("savedefault\n"
                               "load_video\n"
                               "set gfx_payload=keep\n"
                               "insmod gzio\n"
                               GRUB_LINUX_CMD " %s%s%s%s%s%s%s%s\n"
                               "%s%s%s%s%s",
                               GRUB_BOOT_DEVICE, clinux,
                               root_prepend ? " " : "", root_prepend ? root_prepend : "",
                               options ? " " : "", options ? options : "",
                               kparams_env ? " " : "", kparams_env ? kparams_env : "",
                               initrd ? GRUB_INITRD_CMD " " : "", initrd ? GRUB_BOOT_DEVICE : "", initrd ? initrd : "",
                               (initrd && ctx->initramfs_append) ? " " GRUB_APPENDED_INITRAMFS_PATH GRUB_APPENDED_INITRAMFS_FILE : "",
                               initrd ? "\n" : "");

finish:
  grub_free (p);
  grub_free (root_prepend);
  grub_free (title);
  grub_free (options);
  grub_free (clinux);
  grub_free (initrd);

  if (f)
    grub_file_close (f);

  return 0;
}

static void
build_menu (struct boot_entry *boot_entries)
{
  char *submenu = NULL;
  const char *args[2] = { NULL, NULL };
  struct boot_entry *entry = boot_entries;

  if (!boot_entries[0].src)
    return;

  /*
   * [ Main entry ]
   */

  args[0] = MAIN_ENTRY_TITLE;
  grub_normal_add_menu_entry (1, args, NULL, NULL, "bls", NULL, NULL, boot_entries[0].src, 0);

  /*
   * [ Submenu ]
   */

  /* Check if a submenu is needed */
  if (boot_entries[1].src)
    {
      while ((entry < boot_entries + MAX_ENTRIES) && entry->src)
        {
          char *p = submenu;
          submenu = grub_xasprintf ("%s"
                                    "menuentry '%s' { %s }\n",
                                    submenu ? submenu : "",
                                    entry->title, entry->src);
          grub_free (p);
          grub_free (entry->title);
          grub_free (entry->src);
          entry++;
        }

      args[0] = SUBMENU_TITLE;
      grub_normal_add_menu_entry (1, args, NULL, NULL, "bls", NULL, NULL, submenu, 1);

      grub_free (submenu);
    }
  else
    {
      grub_free (boot_entries[0].title);
      grub_free (boot_entries[0].src);
    }
}

static int
find_file (const char *cur_filename, const struct grub_dirhook_info *info,
	   void *data)
{
  int *file_exists = data;

  if ((info->case_insensitive ? grub_strcasecmp (cur_filename, GRUB_APPENDED_INITRAMFS_FILE)
       : grub_strcmp (cur_filename, GRUB_APPENDED_INITRAMFS_FILE)) == 0)
    {
      *file_exists = 1;
      return 1;
    }
  return 0;
}

static grub_err_t
grub_cmd_bls_import (grub_extcmd_context_t ctxt __attribute__ ((unused)),
		     int argc __attribute__ ((unused)),
		     char **args __attribute__ ((unused)))
{
  grub_fs_t fs;
  grub_device_t dev;
  static grub_err_t r;
  const char *devid;
  struct parse_entry_ctx ctx;

  grub_memset (&ctx, 0, sizeof (ctx));

  devid = grub_env_get ("root");
  if (!devid)
    return grub_error (GRUB_ERR_FILE_NOT_FOUND, N_("variable `%s' isn't set"), "root");

  dev = grub_device_open (devid);
  if (!dev)
    return grub_errno;

  fs = grub_fs_probe (dev);
  if (!fs)
    {
      r = grub_errno;
      goto finish;
    }

  /* Check for appended initramfs */
  r = fs->dir (dev, GRUB_APPENDED_INITRAMFS_PATH, find_file, &ctx.initramfs_append);

  /* Menu */
  r = fs->dir (dev, GRUB_BLS_CONFIG_PATH, parse_entry, &ctx);
  build_menu (ctx.boot_entries);

finish:
  if (dev)
    grub_device_close (dev);

  return r;
}

static grub_extcmd_t cmd;

GRUB_MOD_INIT(bls)
{
  cmd = grub_register_extcmd ("bls_import",
			      grub_cmd_bls_import,
			      0,
			      NULL,
			      N_("Import Boot Loader Specification snippets."),
			      NULL);
}

GRUB_MOD_FINI(bls)
{
  grub_unregister_extcmd (cmd);
}
