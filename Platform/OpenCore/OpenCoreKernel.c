/** @file
  OpenCore driver.

Copyright (c) 2019, vit9696. All rights reserved.<BR>
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <OpenCore.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcAppleKernelLib.h>
#include <Library/OcMiscLib.h>
#include <Library/OcStringLib.h>
#include <Library/OcVirtualFsLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/DevicePath.h>

STATIC OC_STORAGE_CONTEXT  *mOcStorage;
STATIC OC_GLOBAL_CONFIG    *mOcConfiguration;

STATIC
VOID
OcKernelReadDarwinVersion (
  IN  CONST UINT8   *Kernel,
  IN  UINT32        KernelSize,
  OUT CHAR8         *DarwinVersion,
  OUT UINT32        DarwinVersionSize
  )
{
  INT32   Offset;
  UINT32  Index;

  ASSERT (DarwinVersion > 0);

  Offset = FindPattern (
    (CONST UINT8 *) "Darwin Kernel Version ",
    NULL,
    L_STR_LEN ("Darwin Kernel Version "),
    Kernel,
    KernelSize,
    0
    );

  if (Offset < 0) {
    DEBUG ((DEBUG_WARN, "OC: Failed to determine kernel version\n"));
    DarwinVersion[0] = '\0';
    return;
  }

  for (Index = 0; Index < DarwinVersionSize - 1; ++Index, ++Offset) {
    if (Offset >= KernelSize || Kernel[Offset] == ':') {
      break;
    }
    DarwinVersion[Index] = (CHAR8) Kernel[Offset];
  }
  DarwinVersion[Index] = '\0';

  DEBUG ((DEBUG_INFO, "OC: Read kernel version %a\n", DarwinVersion));
}

STATIC
UINT32
OcKernelLoadKextsAndReserve (
  IN OC_STORAGE_CONTEXT  *Storage,
  IN OC_GLOBAL_CONFIG    *Config
  )
{
  UINT32               Index;
  UINT32               ReserveSize;
  CHAR8                *BundleName;
  CHAR8                *PlistPath;
  CHAR8                *ExecutablePath;
  CHAR16               FullPath[128];
  OC_KERNEL_ADD_ENTRY  *Kext;

  ReserveSize = PRELINK_INFO_RESERVE_SIZE;

  for (Index = 0; Index < Config->Kernel.Add.Count; ++Index) {
    Kext = Config->Kernel.Add.Values[Index];

    if (Kext->Disabled) {
      continue;
    }

    if (Kext->PlistDataSize == 0) {
      BundleName     = OC_BLOB_GET (&Kext->BundleName);
      PlistPath      = OC_BLOB_GET (&Kext->PlistPath);
      if (BundleName[0] == '\0' || PlistPath[0] == '\0') {
        DEBUG ((DEBUG_ERROR, "OC: Your config has improper for kext info\n"));
        continue;
      }

      UnicodeSPrint (
        FullPath,
        sizeof (FullPath),
        OPEN_CORE_KEXT_PATH "%a\\%a",
        BundleName,
        PlistPath
        );

      UnicodeUefiSlashes (FullPath);

      Kext->PlistData = OcStorageReadFileUnicode (
        Storage,
        FullPath,
        &Kext->PlistDataSize
        );

      if (Kext->PlistData == NULL) {
        DEBUG ((DEBUG_ERROR, "OC: Plist %s is missing for kext %s\n", FullPath, BundleName));
        continue;
      }

      ExecutablePath = OC_BLOB_GET (&Kext->ExecutablePath);
      if (ExecutablePath[0] != '\0') {
        UnicodeSPrint (
          FullPath,
          sizeof (FullPath),
          OPEN_CORE_KEXT_PATH "%a\\%a",
          BundleName,
          ExecutablePath
          );

        UnicodeUefiSlashes (FullPath);

        Kext->ImageData = OcStorageReadFileUnicode (
          Storage,
          FullPath,
          &Kext->ImageDataSize
          );

        if (Kext->ImageData == NULL) {
          DEBUG ((DEBUG_ERROR, "OC: Image %s is missing for kext %s\n", FullPath, BundleName));
          //
          // Still continue loading?
          //
        }
      }
    }

    PrelinkedReserveKextSize (
      &ReserveSize,
      Kext->PlistDataSize,
      Kext->ImageData,
      Kext->ImageDataSize
      );
  }

  DEBUG ((DEBUG_INFO, "Kext reservation size %u\n", ReserveSize));

  return ReserveSize;
}

STATIC
VOID
OcKernelApplyPatches (
  IN     OC_GLOBAL_CONFIG  *Config,
  IN     CONST CHAR8       *DarwinVersion,
  IN     PRELINKED_CONTEXT *Context,
  IN OUT UINT8             *Kernel,
  IN     UINT32            Size
  )
{
  EFI_STATUS             Status;
  PATCHER_CONTEXT        Patcher;
  UINT32                 Index;
  PATCHER_GENERIC_PATCH  Patch;
  OC_KERNEL_PATCH_ENTRY  *UserPatch;
  CONST CHAR8            *Target;
  CONST CHAR8            *MatchKernel;
  BOOLEAN                IsKernelPatch;

  IsKernelPatch = Context == NULL;

  if (IsKernelPatch) {
    ASSERT (Kernel != NULL);

    Status = PatcherInitContextFromBuffer (
      &Patcher,
      Kernel,
      Size
      );

    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "OC: Kernel patcher kernel init failure - %r\n", Status));
      return;
    }
  }

  for (Index = 0; Index < Config->Kernel.Patch.Count; ++Index) {
    UserPatch = Config->Kernel.Patch.Values[Index];
    Target    = OC_BLOB_GET (&UserPatch->Identifier);

    if (UserPatch->Disabled
    || (AsciiStrCmp (Target, "kernel") == 0) != IsKernelPatch) {
      continue;
    }

    MatchKernel = OC_BLOB_GET (&UserPatch->MatchKernel);

    if (AsciiStrnCmp (DarwinVersion, MatchKernel, UserPatch->MatchKernel.Size) != 0) {
      DEBUG ((
        DEBUG_INFO,
        "OC: Kernel patcher skips %a patch at %u due to version %a vs %a",
        Target,
        Index,
        MatchKernel,
        DarwinVersion
        ));
      continue;
    }

    if (!IsKernelPatch) {
      Status = PatcherInitContextFromPrelinked (
        &Patcher,
        Context,
        Target
        );

      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "OC: Kernel patcher %a init failure - %r\n", Target, Status));
        continue;
      }
    }

    //
    // Ignore patch if:
    // - There is nothing to replace.
    // - We have neither symbolic base, nor find data.
    // - Find and replace mismatch in size.
    // - Mask and ReplaceMask mismatch in size when are available.
    //
    if (UserPatch->Replace.Size == 0
      || (UserPatch->Base.Size == 0 && UserPatch->Find.Size != UserPatch->Replace.Size)
      || (UserPatch->Mask.Size > 0 && UserPatch->Find.Size != UserPatch->Mask.Size)
      || (UserPatch->ReplaceMask.Size > 0 && UserPatch->Find.Size != UserPatch->ReplaceMask.Size)) {
      DEBUG ((DEBUG_ERROR, "OC: Kernel patch %u for %a is borked\n", Index, Target));
      continue;
    }

    ZeroMem (&Patch, sizeof (Patch));

    if (UserPatch->Base.Size > 0) {
      Patch.Base  = OC_BLOB_GET (&UserPatch->Base);
    }

    if (UserPatch->Find.Size > 0) {
      Patch.Find  = OC_BLOB_GET (&UserPatch->Find);
    }

    Patch.Replace = OC_BLOB_GET (&UserPatch->Replace);

    if (UserPatch->Mask.Size > 0) {
      Patch.Mask  = OC_BLOB_GET (&UserPatch->Mask);
    }

    if (UserPatch->ReplaceMask.Size > 0) {
      Patch.ReplaceMask = OC_BLOB_GET (&UserPatch->ReplaceMask);
    }

    Patch.Size    = UserPatch->Replace.Size;
    Patch.Count   = UserPatch->Count;
    Patch.Skip    = UserPatch->Skip;
    Patch.Limit   = UserPatch->Limit;

    Status = PatcherApplyGenericPatch (&Patcher, &Patch);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "OC: Kernel patcher failed %u for %a - %r\n", Index, Target, Status));
    }
  }

  if (!IsKernelPatch) {
    if (Config->Kernel.Quirks.AppleCpuPmCfgLock) {
      PatchAppleIntelCPUPowerManagement (Context);
    }

    if (Config->Kernel.Quirks.ExternalDiskIcons) {
      PatchForceInternalDiskIcons (Context);
    }

    if (Config->Kernel.Quirks.ThirdPartyTrim) {
      PatchThirdPartySsdTrim (Context);
    }

    if (Config->Kernel.Quirks.XhciPortLimit) {
      PatchUsbXhciPortLimit (Context);
    }
  }
}

STATIC
VOID
OcKernelBlockKexts (
  IN     OC_GLOBAL_CONFIG  *Config,
  IN     CONST CHAR8       *DarwinVersion,
  IN     PRELINKED_CONTEXT *Context
  )
{
  EFI_STATUS             Status;
  PATCHER_CONTEXT        Patcher;
  UINT32                 Index;
  OC_KERNEL_BLOCK_ENTRY  *Kext;
  CONST CHAR8            *Target;
  CONST CHAR8            *MatchKernel;

  for (Index = 0; Index < Config->Kernel.Block.Count; ++Index) {
    Kext   = Config->Kernel.Block.Values[Index];
    Target = OC_BLOB_GET (&Kext->Identifier);

    if (Kext->Disabled) {
      continue;
    }

    MatchKernel = OC_BLOB_GET (&Kext->MatchKernel);

    if (AsciiStrnCmp (DarwinVersion, MatchKernel, Kext->MatchKernel.Size) != 0) {
      DEBUG ((
        DEBUG_INFO,
        "OC: Kernel blocker skips %a block at %u due to version %a vs %a",
        Target,
        Index,
        MatchKernel,
        DarwinVersion
        ));
      continue;
    }

    Status = PatcherInitContextFromPrelinked (
      &Patcher,
      Context,
      Target
      );

    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "OC: Kernel blocker %a init failure - %r\n", Target, Status));
      continue;
    }

    Status = PatcherBlockKext (&Patcher);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "OC: Kernel blocker %a failed - %r\n", Target, Status));
    }
  }
}

STATIC
EFI_STATUS
OcKernelProcessPrelinked (
  IN     OC_GLOBAL_CONFIG  *Config,
  IN     CONST CHAR8       *DarwinVersion,
  IN OUT UINT8             *Kernel,
  IN     UINT32            *KernelSize,
  IN     UINT32            AllocatedSize
  )
{
  EFI_STATUS           Status;
  PRELINKED_CONTEXT    Context;
  CHAR8                *BundleName;
  CHAR8                *ExecutablePath;
  UINT32               Index;
  CHAR8                FullPath[128];
  OC_KERNEL_ADD_ENTRY  *Kext;
  CONST CHAR8          *MatchKernel;

  Status = PrelinkedContextInit (&Context, Kernel, *KernelSize, AllocatedSize);

  if (!EFI_ERROR (Status)) {
    OcKernelApplyPatches (Config, DarwinVersion, &Context, NULL, 0);

    OcKernelBlockKexts (Config, DarwinVersion, &Context);

    Status = PrelinkedInjectPrepare (&Context);
    if (!EFI_ERROR (Status)) {

      for (Index = 0; Index < Config->Kernel.Add.Count; ++Index) {
        Kext = Config->Kernel.Add.Values[Index];

        if (Kext->Disabled || Kext->PlistDataSize == 0) {
          continue;
        }

        BundleName     = OC_BLOB_GET (&Kext->BundleName);
        MatchKernel = OC_BLOB_GET (&Kext->MatchKernel);

        if (AsciiStrnCmp (DarwinVersion, MatchKernel, Kext->MatchKernel.Size) != 0) {
          DEBUG ((
            DEBUG_INFO,
            "OC: Prelink injection skips %a kext at %u due to version %a vs %a",
            BundleName,
            Index,
            MatchKernel,
            DarwinVersion
            ));
          continue;
        }

        AsciiSPrint (FullPath, sizeof (FullPath), "/Library/Extensions/%a", BundleName);
        if (Kext->ImageData != NULL) {
          ExecutablePath = OC_BLOB_GET (&Kext->ExecutablePath);
        } else {
          ExecutablePath = NULL;
        }

        Status = PrelinkedInjectKext (
          &Context,
          FullPath,
          Kext->PlistData,
          Kext->PlistDataSize,
          ExecutablePath,
          Kext->ImageData,
          Kext->ImageDataSize
          );

        DEBUG ((DEBUG_INFO, "OC: Prelink injection %a - %r\n", BundleName, Status));
      }

      Status = PrelinkedInjectComplete (&Context);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_WARN, "OC: Prelink insertion error - %r\n", Status));
      }
    } else {
      DEBUG ((DEBUG_WARN, "OC: Prelink inject prepare error - %r\n", Status));
    }

    *KernelSize = Context.PrelinkedSize;

    PrelinkedContextFree (&Context);
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
OcKernelFileOpen (
  IN  EFI_FILE_PROTOCOL       *This,
  OUT EFI_FILE_PROTOCOL       **NewHandle,
  IN  CHAR16                  *FileName,
  IN  UINT64                  OpenMode,
  IN  UINT64                  Attributes
  )
{
  EFI_STATUS         Status;
  UINT8              *Kernel;
  UINT32             KernelSize;
  UINT32             AllocatedSize;
  CHAR16             *FileNameCopy;
  EFI_FILE_PROTOCOL  *VirtualFileHandle;
  EFI_STATUS         PrelinkedStatus;
  EFI_TIME           ModificationTime;
  CHAR8              DarwinVersion[16];

  Status = This->Open (This, NewHandle, FileName, OpenMode, Attributes);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // boot.efi uses /S/L/K/kernel as is to determine valid filesystem.
  // Just skip it to speedup the boot process.
  // On 10.9 mach_kernel is loaded for manual linking aferwards, so we cannot skip it.
  //
  if (OpenMode == EFI_FILE_MODE_READ
    && StrStr (FileName, L"kernel") != NULL
    && StrCmp (FileName, L"System\\Library\\Kernels\\kernel") != 0) {

    DEBUG ((DEBUG_INFO, "Trying XNU hook on %s\n", FileName));
    Status = ReadAppleKernel (
      *NewHandle,
      &Kernel,
      &KernelSize,
      &AllocatedSize,
      OcKernelLoadKextsAndReserve (mOcStorage, mOcConfiguration)
      );
    DEBUG ((DEBUG_INFO, "Result of XNU hook on %s is %r\n", FileName, Status));

    //
    // This is not Apple kernel, just return the original file.
    //
    if (!EFI_ERROR (Status)) {
      OcKernelReadDarwinVersion (Kernel, KernelSize, DarwinVersion, sizeof (DarwinVersion));
      OcKernelApplyPatches (mOcConfiguration, DarwinVersion, NULL, Kernel, KernelSize);

      PrelinkedStatus = OcKernelProcessPrelinked (
        mOcConfiguration,
        DarwinVersion,
        Kernel,
        &KernelSize,
        AllocatedSize
        );

      DEBUG ((DEBUG_INFO, "Prelinked status - %r\n", PrelinkedStatus));

      Status = GetFileModifcationTime (*NewHandle, &ModificationTime);
      if (EFI_ERROR (Status)) {
        ZeroMem (&ModificationTime, sizeof (ModificationTime));
      }

      (*NewHandle)->Close(*NewHandle);

      //
      // This was our file, yet firmware is dying.
      //
      FileNameCopy = AllocateCopyPool (StrSize (FileName), FileName);
      if (FileNameCopy == NULL) {
        DEBUG ((DEBUG_WARN, "Failed to allocate kernel name (%a) copy\n", FileName));
        FreePool (Kernel);
        return EFI_OUT_OF_RESOURCES;
      }

      Status = CreateVirtualFile (FileNameCopy, Kernel, KernelSize, &ModificationTime, &VirtualFileHandle);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_WARN, "Failed to virtualise kernel file (%a)\n", FileName));
        FreePool (Kernel);
        FreePool (FileNameCopy);
        return EFI_OUT_OF_RESOURCES;
      }

      //
      // Return our handle.
      //
      *NewHandle = VirtualFileHandle;
      return EFI_SUCCESS;
    }
  }

  return CreateRealFile (*NewHandle, NULL, TRUE, NewHandle);
}

VOID
OcLoadKernelSupport (
  IN OC_STORAGE_CONTEXT  *Storage,
  IN OC_GLOBAL_CONFIG    *Config
  )
{
  EFI_STATUS  Status;

  Status = EnableVirtualFs (gBS, OcKernelFileOpen);

  if (!EFI_ERROR (Status)) {
    mOcStorage       = Storage;
    mOcConfiguration = Config;
  } else {
    DEBUG ((DEBUG_ERROR, "OC: Failed to enable vfs - %r\n", Status));
  }
}

VOID
OcUnloadKernelSupport (
  VOID
  )
{
  EFI_STATUS  Status;

  if (mOcStorage != NULL) {
    Status = DisableVirtualFs (gBS);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "OC: Failed to disable vfs - %r\n", Status));
    }
    mOcStorage       = NULL;
    mOcConfiguration = NULL;
  }
}