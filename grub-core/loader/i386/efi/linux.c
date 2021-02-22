/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2012  Free Software Foundation, Inc.
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

#include <grub/loader.h>
#include <grub/file.h>
#include <grub/err.h>
#include <grub/misc.h>
#include <grub/types.h>
#include <grub/mm.h>
#include <grub/cpu/linux.h>
#include <grub/command.h>
#include <grub/i18n.h>
#include <grub/lib/cmdline.h>
#include <grub/linux.h>
#include <grub/efi/efi.h>
#include <stddef.h>

GRUB_MOD_LICENSE ("GPLv3+");

static grub_dl_t my_mod;
static int loaded;
static void *kernel_mem;
static grub_uint64_t kernel_size;
static grub_uint8_t *initrd_mem;
static grub_uint32_t handover_offset;
struct linux_kernel_params *params;
static char *linux_cmdline;

#define BYTES_TO_PAGES(bytes)   (((bytes) + 0xfff) >> 12)

#define SHIM_LOCK_GUID \
  { 0x605dab50, 0xe046, 0x4300, {0xab, 0xb6, 0x3d, 0xd8, 0x10, 0xdd, 0x8b, 0x23} }

struct grub_efi_shim_lock
{
  grub_efi_status_t (*verify) (void *buffer, grub_uint32_t size);
};
typedef struct grub_efi_shim_lock grub_efi_shim_lock_t;

static grub_efi_boolean_t
grub_linuxefi_secure_validate (void *data, grub_uint32_t size)
{
  grub_efi_guid_t guid = SHIM_LOCK_GUID;
  grub_efi_shim_lock_t *shim_lock;
  grub_efi_status_t status;

  if (! grub_efi_secure_boot())
    {
      grub_dprintf ("linuxefi", "secure boot not enabled, not validating");
      return 1;
    }

  grub_dprintf ("linuxefi", "Locating shim protocol\n");
  shim_lock = grub_efi_locate_protocol(&guid, NULL);

  if (!shim_lock)
    {
      grub_dprintf ("linuxefi", "shim not available\n");
      return 0;
    }

  grub_dprintf ("linuxefi", "Asking shim to verify kernel signature\n");
  status = shim_lock->verify(data, size);
  if (status == GRUB_EFI_SUCCESS)
    {
      grub_dprintf ("linuxefi", "Kernel signature verification passed\n");
      return 1;
    }

  grub_dprintf ("linuxefi", "Kernel signature verification failed (0x%lx)\n",
		(unsigned long) status);
  return 0;
}

typedef void(*handover_func)(void *, grub_efi_system_table_t *, struct linux_kernel_params *);

static grub_err_t
grub_linuxefi_boot (void)
{
  handover_func hf;
  int offset = 0;

#ifdef __x86_64__
  offset = 512;
#endif

  hf = (handover_func)((char *)kernel_mem + handover_offset + offset);

  asm volatile ("cli");

  hf (grub_efi_image_handle, grub_efi_system_table, params);

  /* Not reached */
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_linuxefi_unload (void)
{
  grub_dl_unref (my_mod);
  loaded = 0;
  if (initrd_mem)
    grub_efi_free_pages((grub_efi_physical_address_t)(grub_addr_t)initrd_mem, BYTES_TO_PAGES(params->ramdisk_size));
  if (linux_cmdline)
    grub_efi_free_pages((grub_efi_physical_address_t)(grub_addr_t)linux_cmdline, BYTES_TO_PAGES(params->cmdline_size + 1));
  if (kernel_mem)
    grub_efi_free_pages((grub_efi_physical_address_t)(grub_addr_t)kernel_mem, BYTES_TO_PAGES(kernel_size));
  if (params)
    grub_efi_free_pages((grub_efi_physical_address_t)(grub_addr_t)params, BYTES_TO_PAGES(16384));
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_initrd (grub_command_t cmd __attribute__ ((unused)),
                 int argc, char *argv[])
{
  grub_size_t size = 0;
  struct grub_linux_initrd_context initrd_ctx;

  if (argc == 0)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));
      goto fail;
    }

  if (!loaded)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, N_("you need to load the kernel first"));
      goto fail;
    }

  if (grub_initrd_init (argc, argv, &initrd_ctx))
    goto fail;

  size = grub_get_initrd_size (&initrd_ctx);

  initrd_mem = grub_efi_allocate_pages_max (0x3fffffff, BYTES_TO_PAGES(size));

  if (!initrd_mem)
    {
      grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("can't allocate initrd"));
      goto fail;
    }

  grub_dprintf ("linuxefi", "initrd_mem = %lx\n", (unsigned long) initrd_mem);

  params->ramdisk_size = size;
  params->ramdisk_image = (grub_uint32_t)(grub_addr_t) initrd_mem;

  if (grub_initrd_load (&initrd_ctx, argv, initrd_mem))
    goto fail;

  params->ramdisk_size = size;

 fail:
  grub_initrd_close (&initrd_ctx);

  if (initrd_mem && grub_errno)
    grub_efi_free_pages((grub_efi_physical_address_t)(grub_addr_t)initrd_mem, BYTES_TO_PAGES(size));

  return grub_errno;
}

static grub_err_t
grub_cmd_linux (grub_command_t cmd __attribute__ ((unused)),
		int argc, char *argv[])
{
  grub_file_t file = 0;
  struct linux_i386_kernel_header lh;
  grub_ssize_t len, start, filelen;
  void *kernel;

  grub_dl_ref (my_mod);

  if (argc == 0)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));
      goto fail;
    }

  file = grub_file_open (argv[0], GRUB_FILE_TYPE_LINUX_KERNEL);
  if (! file)
    goto fail;

  filelen = grub_file_size (file);

  kernel = grub_malloc(filelen);

  if (!kernel)
    {
      grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("cannot allocate kernel buffer"));
      goto fail;
    }

  if (grub_file_read (file, kernel, filelen) != filelen)
    {
      grub_error (GRUB_ERR_FILE_READ_ERROR, N_("Can't read kernel %s"), argv[0]);
      goto fail;
    }

  if (! grub_linuxefi_secure_validate (kernel, filelen))
    {
      grub_error (GRUB_ERR_ACCESS_DENIED, N_("%s has invalid signature"), argv[0]);
      grub_free (kernel);
      goto fail;
    }

  grub_file_seek (file, 0);

  grub_free(kernel);

  params = grub_efi_allocate_pages_max (0x3fffffff, BYTES_TO_PAGES(16384));

  if (! params)
    {
      grub_error (GRUB_ERR_OUT_OF_MEMORY, "cannot allocate kernel parameters");
      goto fail;
    }

  grub_dprintf ("linuxefi", "params = %lx\n", (unsigned long) params);

  grub_memset (params, 0, 16384);

  if (grub_file_read (file, &lh, sizeof (lh)) != sizeof (lh))
    {
      if (!grub_errno)
	grub_error (GRUB_ERR_BAD_OS, N_("premature end of file %s"),
		    argv[0]);
      goto fail;
    }

  if (lh.boot_flag != grub_cpu_to_le16 (0xaa55))
    {
      grub_error (GRUB_ERR_BAD_OS, N_("invalid magic number"));
      goto fail;
    }

  if (lh.setup_sects > GRUB_LINUX_MAX_SETUP_SECTS)
    {
      grub_error (GRUB_ERR_BAD_OS, N_("too many setup sectors"));
      goto fail;
    }

  if (lh.version < grub_cpu_to_le16 (0x020b))
    {
      grub_error (GRUB_ERR_BAD_OS, N_("kernel too old"));
      goto fail;
    }

  if (!lh.handover_offset)
    {
      grub_error (GRUB_ERR_BAD_OS, N_("kernel doesn't support EFI handover"));
      goto fail;
    }

  linux_cmdline = grub_efi_allocate_pages_max(0x3fffffff,
					 BYTES_TO_PAGES(lh.cmdline_size + 1));

  if (!linux_cmdline)
    {
      grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("can't allocate cmdline"));
      goto fail;
    }

  grub_dprintf ("linuxefi", "linux_cmdline = %lx\n",
		(unsigned long) linux_cmdline);

  grub_memcpy (linux_cmdline, LINUX_IMAGE, sizeof (LINUX_IMAGE));
  {
    grub_err_t err;
    err = grub_create_loader_cmdline (argc, argv,
				      linux_cmdline
				      + sizeof (LINUX_IMAGE) - 1,
				      lh.cmdline_size
				      - (sizeof (LINUX_IMAGE) - 1),
				      GRUB_VERIFY_KERNEL_CMDLINE);
    if (err)
      goto fail;
  }

  lh.cmd_line_ptr = (grub_uint32_t)(grub_addr_t)linux_cmdline;

  handover_offset = lh.handover_offset;

  start = (lh.setup_sects + 1) * 512;
  len = grub_file_size(file) - start;

  kernel_mem = grub_efi_allocate_fixed(lh.pref_address,
				       BYTES_TO_PAGES(lh.init_size));

  if (!kernel_mem)
    kernel_mem = grub_efi_allocate_pages_max(0x3fffffff,
					     BYTES_TO_PAGES(lh.init_size));

  if (!kernel_mem)
    {
      grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("can't allocate kernel"));
      goto fail;
    }

  grub_dprintf ("linuxefi", "kernel_mem = %lx\n", (unsigned long) kernel_mem);

  if (grub_file_seek (file, start) == (grub_off_t) -1)
    {
      grub_error (GRUB_ERR_BAD_OS, N_("premature end of file %s"),
		  argv[0]);
      goto fail;
    }

  if (grub_file_read (file, kernel_mem, len) != len && !grub_errno)
    {
      grub_error (GRUB_ERR_BAD_OS, N_("premature end of file %s"),
		  argv[0]);
    }

  if (grub_errno == GRUB_ERR_NONE)
    {
      grub_loader_set (grub_linuxefi_boot, grub_linuxefi_unload, 0);
      loaded = 1;
      lh.code32_start = (grub_uint32_t)(grub_addr_t) kernel_mem;
    }

  /* do not overwrite below boot_params->hdr to avoid setting the sentinel byte */
  start = offsetof (struct linux_kernel_params, setup_sects);
  grub_memcpy ((grub_uint8_t *)params + start, (grub_uint8_t *)&lh + start, 2 * 512 - start);

  params->type_of_loader = 0x21;

 fail:

  if (file)
    grub_file_close (file);

  if (grub_errno != GRUB_ERR_NONE)
    {
      grub_dl_unref (my_mod);
      loaded = 0;
    }

  if (linux_cmdline && !loaded)
    grub_efi_free_pages((grub_efi_physical_address_t)(grub_addr_t)linux_cmdline, BYTES_TO_PAGES(lh.cmdline_size + 1));

  if (kernel_mem && !loaded)
    grub_efi_free_pages((grub_efi_physical_address_t)(grub_addr_t)kernel_mem, BYTES_TO_PAGES(kernel_size));

  if (params && !loaded)
    grub_efi_free_pages((grub_efi_physical_address_t)(grub_addr_t)params, BYTES_TO_PAGES(16384));

  return grub_errno;
}

static grub_command_t cmd_linux, cmd_initrd;

GRUB_MOD_INIT(linuxefi)
{
  cmd_linux =
    grub_register_command ("linuxefi", grub_cmd_linux,
                           0, N_("Load Linux."));
  cmd_initrd =
    grub_register_command ("initrdefi", grub_cmd_initrd,
                           0, N_("Load initrd."));
  my_mod = mod;
}

GRUB_MOD_FINI(linuxefi)
{
  grub_unregister_command (cmd_linux);
  grub_unregister_command (cmd_initrd);
}
