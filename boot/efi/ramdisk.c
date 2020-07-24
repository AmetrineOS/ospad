/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck_gen_headers/config_boot.h>

#include <tilck/common/basic_defs.h>
#include <tilck/common/page_size.h>
#include <tilck/common/failsafe_assert.h>
#include <tilck/common/fat32_base.h>
#include <tilck/common/utils.h>

/* We HAVE to undef our ASSERT because the gnu-efi headers define it */
#undef ASSERT

#include <efi.h>
#include <efilib.h>
#include <multiboot.h>
#include <efierr.h>

#include "utils.h"

#define LOADING_RAMDISK_STR            L"Loading ramdisk... "

struct load_ramdisk_ctx {

   EFI_BLOCK_IO_PROTOCOL *blockio;

   UINT32 total_fat_size;
   UINT32 rounded_tot_fat_sz;       /* Rounded up at PAGE_SIZE */

   UINT32 tot_used_bytes;
   UINT32 rounded_tot_used_bytes;   /* Rounded up at PAGE_SIZE */

   void *fat_hdr;
};

static EFI_STATUS
LoadRamdisk_GetTotFatSize(struct load_ramdisk_ctx *ctx)
{
   const UINTN initrd_off = INITRD_SECTOR * SECTOR_SIZE;
   EFI_PHYSICAL_ADDRESS paddr = 0;
   EFI_STATUS status;
   UINT32 fat_sec_sz;
   void *fat_hdr;

   status = BS->AllocatePages(AllocateAnyPages,
                              EfiLoaderData,
                              1, /* just 1 page */
                              &paddr);
   HANDLE_EFI_ERROR("AllocatePages");
   fat_hdr = TO_PTR(paddr);

   status = ReadAlignedBlock(ctx->blockio, initrd_off, PAGE_SIZE, fat_hdr);
   HANDLE_EFI_ERROR("ReadAlignedBlock");

   fat_sec_sz = fat_get_sector_size(fat_hdr);
   ctx->total_fat_size = (fat_get_first_data_sector(fat_hdr) + 1) * fat_sec_sz;
   ctx->rounded_tot_fat_sz = round_up_at(ctx->total_fat_size, PAGE_SIZE);

   status = BS->FreePages(paddr, 1);
   HANDLE_EFI_ERROR("FreePages");

end:
   return status;
}

static EFI_STATUS
LoadRamdisk_GetTotUsedBytes(struct load_ramdisk_ctx *ctx)
{
   const UINTN initrd_off = INITRD_SECTOR * SECTOR_SIZE;
   EFI_PHYSICAL_ADDRESS paddr = 0;
   EFI_STATUS status;
   void *fat_hdr;

   /* Allocate memory for storing the whole FAT table */
   status = BS->AllocatePages(AllocateAnyPages,
                              EfiLoaderData,
                              ctx->rounded_tot_fat_sz / PAGE_SIZE,
                              &paddr);
   HANDLE_EFI_ERROR("AllocatePages");
   fat_hdr = TO_PTR(paddr);

   status = ReadAlignedBlock(ctx->blockio,
                             initrd_off,
                             ctx->total_fat_size,
                             fat_hdr);
   HANDLE_EFI_ERROR("ReadAlignedBlock");

   ctx->tot_used_bytes = fat_calculate_used_bytes(fat_hdr);
   ctx->rounded_tot_used_bytes = round_up_at(ctx->tot_used_bytes, PAGE_SIZE);

   /*
    * Now we know everything. Free the memory used so far and allocate the
    * big buffer to store all the "used" clusters of the FAT32 partition,
    * including clearly the header and the FAT table.
    */

   status = BS->FreePages(paddr, ctx->rounded_tot_fat_sz / PAGE_SIZE);
   HANDLE_EFI_ERROR("FreePages");

end:
   return status;
}

static EFI_STATUS
LoadRamdisk_CompactClusters(struct load_ramdisk_ctx *ctx)
{
   EFI_STATUS status = EFI_SUCCESS;
   void *fat_hdr = ctx->fat_hdr;
   UINT32 ff_clu_off;

   ff_clu_off = fat_get_first_free_cluster_off(fat_hdr);

   if (ff_clu_off < ctx->tot_used_bytes) {

      Print(L"Compacting ramdisk... ");

      fat_compact_clusters(fat_hdr);
      ff_clu_off = fat_get_first_free_cluster_off(fat_hdr);
      ctx->tot_used_bytes = fat_calculate_used_bytes(fat_hdr);

      if (ctx->tot_used_bytes != ff_clu_off) {

         Print(L"fat_compact_clusters failed: %u != %u\r\n",
               ctx->tot_used_bytes, ff_clu_off);

         status = EFI_ABORTED;
         goto end;
      }

      Print(L"[ OK ]\r\n");
   }

end:
   return status;
}

static EFI_STATUS
LoadRamdisk_AllocMem(struct load_ramdisk_ctx *ctx)
{
   EFI_STATUS status;
   EFI_PHYSICAL_ADDRESS paddr;

   /*
    * Because Tilck is 32-bit and it maps the first LINEAR_MAPPING_SIZE of
    * physical memory at KERNEL_BASE_VA, we really cannot accept ANY address
    * in the 64-bit space, because from Tilck we won't be able to read from
    * there. The address of the ramdisk we actually be at most:
    *
    *    LINEAR_MAPPING_SIZE - "size of ramdisk"
    *
    * The AllocateMaxAddress allocation type is exactly what we need to use in
    * this case. As explained in the UEFI specification:
    *
    *    Allocation requests of Type AllocateMaxAddress allocate any available
    *    range of pages whose uppermost address is less than or equal to the
    *    address pointed to by Memory on input.
    *
    * Additional notes
    * --------------------------
    * Note the `(ctx.rounded_tot_used_bytes / PAGE_SIZE) + 1` expression below.
    *
    *    +1 page is allocated to allow, evenutally, the Tilck kernel
    *    to align it's data clusters at page boundary.
    */

   paddr = LINEAR_MAPPING_SIZE;
   status = BS->AllocatePages(AllocateMaxAddress,
                              EfiLoaderData,
                              (ctx->rounded_tot_used_bytes / PAGE_SIZE) + 1,
                              &paddr);

   HANDLE_EFI_ERROR("AllocatePages");
   ctx->fat_hdr = TO_PTR(paddr);

end:
   return status;
}

EFI_STATUS
LoadRamdisk(EFI_SYSTEM_TABLE *ST,
            EFI_HANDLE image,
            EFI_LOADED_IMAGE *loaded_image,
            EFI_PHYSICAL_ADDRESS *ramdisk_paddr_ref,
            UINTN *ramdisk_size,
            UINTN CurrConsoleRow)
{
   const UINTN initrd_off = INITRD_SECTOR * SECTOR_SIZE;
   EFI_STATUS status = EFI_SUCCESS;
   EFI_DEVICE_PATH *parentDpCopy = NULL;
   EFI_HANDLE parentDpHandle = NULL;
   struct load_ramdisk_ctx ctx = {0};

   parentDpCopy = GetCopyOfParentDevicePathNode(
      DevicePathFromHandle(loaded_image->DeviceHandle)
   );

   status = GetHandlerForDevicePath(parentDpCopy,
                                    &BlockIoProtocol,
                                    &parentDpHandle);
   HANDLE_EFI_ERROR("GetHandlerForDevicePath");

   FreePool(parentDpCopy);
   parentDpCopy = NULL;

   status = BS->OpenProtocol(parentDpHandle,
                             &BlockIoProtocol,
                             (void **)&ctx.blockio,
                             image,
                             NULL,
                             EFI_OPEN_PROTOCOL_GET_PROTOCOL);
   HANDLE_EFI_ERROR("OpenProtocol(BlockIoProtocol)");

   Print(LOADING_RAMDISK_STR);

   status = LoadRamdisk_GetTotFatSize(&ctx);
   HANDLE_EFI_ERROR("LoadRamdisk_GetTotFatSize");

   status = LoadRamdisk_GetTotUsedBytes(&ctx);
   HANDLE_EFI_ERROR("LoadRamdisk_GetTotUsedBytes");

   status = LoadRamdisk_AllocMem(&ctx);
   HANDLE_EFI_ERROR("LoadRamdisk_AllocMem");

   status = ReadDiskWithProgress(ST->ConOut,
                                 CurrConsoleRow,
                                 LOADING_RAMDISK_STR,
                                 ctx.blockio,
                                 initrd_off,
                                 ctx.rounded_tot_used_bytes,
                                 ctx.fat_hdr);
   HANDLE_EFI_ERROR("ReadDiskWithProgress");

   /* Now we're done with the BlockIoProtocol, close it. */
   BS->CloseProtocol(parentDpHandle, &BlockIoProtocol, image, NULL);
   ctx.blockio = NULL;

   ST->ConOut->SetCursorPosition(ST->ConOut, 0, CurrConsoleRow);
   Print(LOADING_RAMDISK_STR);
   Print(L"[ OK ]\r\n");

   status = LoadRamdisk_CompactClusters(&ctx);
   HANDLE_EFI_ERROR("LoadRamdisk_CompactClusters");

   /*
    * Pass via multiboot 'used bytes' as RAMDISK size instead of the real
    * RAMDISK size. This is useful if the kernel uses the RAMDISK read-only.
    *
    * Note[1]: we've increased the value by 1 page in order to allow Tilck's
    * kernel to align the first data sector, if necessary.
    *
    * Note[2]: previously we allocated one additional page.
    */

   *ramdisk_size = ctx.tot_used_bytes + PAGE_SIZE;

   /* Return (as OUR param) a pointer to the ramdisk */
   *ramdisk_paddr_ref = (UINTN)ctx.fat_hdr;

end:

   if (parentDpCopy)
      FreePool(parentDpCopy);

   if (ctx.blockio)
      BS->CloseProtocol(parentDpHandle, &BlockIoProtocol, image, NULL);

   return status;
}
