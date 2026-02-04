#include <Uefi.h>

#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>

#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

#define MAX_LINE_CHARS   128
#define COPY_CHUNK_SIZE  4096

STATIC EFI_FILE_PROTOCOL *mRoot = NULL;

typedef enum {
  MENU_CREATE = 0,
  MENU_DELETE,
  MENU_READ,
  MENU_COPY,
  MENU_MERGE,
  MENU_EXIT,
  MENU_COUNT
} MENU_ITEM;

//
// ---------------- Console Helpers ----------------
//
STATIC VOID Clear(VOID) {
  gST->ConOut->ClearScreen(gST->ConOut);
  gST->ConOut->SetCursorPosition(gST->ConOut, 0, 0);
}

STATIC VOID WaitKey(VOID) {
  UINTN Index;
  gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &Index);
}

STATIC EFI_STATUS ReadKey(EFI_INPUT_KEY *Key) {
  return gST->ConIn->ReadKeyStroke(gST->ConIn, Key);
}

STATIC VOID SetAttrNormal(VOID) { gST->ConOut->SetAttribute(gST->ConOut, EFI_LIGHTGRAY); }
STATIC VOID SetAttrTitle(VOID)  { gST->ConOut->SetAttribute(gST->ConOut, EFI_WHITE | EFI_BACKGROUND_GREEN); }
STATIC VOID SetAttrSelect(VOID) { gST->ConOut->SetAttribute(gST->ConOut, EFI_WHITE | EFI_BACKGROUND_BLUE); }

STATIC VOID Pause(VOID) {
  Print(L"\nPress any key to continue...");
  WaitKey();
  EFI_INPUT_KEY K;
  ReadKey(&K);
}

//
// 讀一行輸入（支援 Backspace / Enter / Esc）
// - Esc 會回 EFI_ABORTED
//
STATIC
EFI_STATUS
GetLine(IN CONST CHAR16 *Prompt, OUT CHAR16 *OutBuf, IN UINTN OutChars)
{
  EFI_INPUT_KEY Key;
  UINTN Len = 0;

  if (OutBuf == NULL || OutChars < 2) return EFI_INVALID_PARAMETER;
  OutBuf[0] = L'\0';

  Print(L"%s", Prompt);

  while (TRUE) {
    WaitKey();
    if (EFI_ERROR(ReadKey(&Key))) continue;

    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      Print(L"\n");
      OutBuf[Len] = L'\0';
      return EFI_SUCCESS;
    }

    if (Key.ScanCode == SCAN_ESC) {
      Print(L"\n");
      OutBuf[0] = L'\0';
      return EFI_ABORTED;
    }

    if (Key.UnicodeChar == CHAR_BACKSPACE) {
      if (Len > 0) {
        Len--;
        OutBuf[Len] = L'\0';
        Print(L"\b \b");
      }
      continue;
    }

    // 可視 ASCII
    if (Key.UnicodeChar >= 0x20 && Key.UnicodeChar <= 0x7E) {
      if (Len + 1 < OutChars) {
        OutBuf[Len++] = Key.UnicodeChar;
        OutBuf[Len] = L'\0';
        Print(L"%c", Key.UnicodeChar);
      }
    }
  }
}

//
// ---------------- FileSystem: Open Root (Robust) ----------------
//
// 1) 先嘗試：從 LoadedImage->DeviceHandle 找 SimpleFS 開 Root
//    - 如果你的 app 不是從 FS0/FS1 載入（例如從 FV 啟動），這裡常會 EFI_NOT_FOUND
//
// 2) 失敗就 fallback：LocateHandleBuffer(ByProtocol SimpleFS) 找第一個可用 FS
//
STATIC
EFI_STATUS
OpenRootFromLoadedImage(IN EFI_HANDLE ImageHandle, OUT EFI_FILE_PROTOCOL **Root)
{
  EFI_STATUS Status;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded = NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfs = NULL;

  if (Root == NULL) return EFI_INVALID_PARAMETER;
  *Root = NULL;

  Status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR(Status)) return Status;

  Status = gBS->HandleProtocol(Loaded->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Sfs);
  if (EFI_ERROR(Status)) return Status;

  return Sfs->OpenVolume(Sfs, Root);
}

STATIC
EFI_STATUS
OpenRootFallbackAnyFs(OUT EFI_FILE_PROTOCOL **Root)
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles = NULL;
  UINTN NoHandles = 0;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfs = NULL;

  if (Root == NULL) return EFI_INVALID_PARAMETER;
  *Root = NULL;

  Status = gBS->LocateHandleBuffer(ByProtocol,
                                   &gEfiSimpleFileSystemProtocolGuid,
                                   NULL,
                                   &NoHandles,
                                   &Handles);
  if (EFI_ERROR(Status) || NoHandles == 0) return EFI_NOT_FOUND;

  Status = gBS->HandleProtocol(Handles[0], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Sfs);
  if (!EFI_ERROR(Status)) {
    Status = Sfs->OpenVolume(Sfs, Root);
  }

  if (Handles) FreePool(Handles);
  return Status;
}

STATIC
EFI_STATUS
OpenRootRobust(IN EFI_HANDLE ImageHandle, OUT EFI_FILE_PROTOCOL **Root)
{
  EFI_STATUS Status;

  Status = OpenRootFromLoadedImage(ImageHandle, Root);
  if (!EFI_ERROR(Status)) return EFI_SUCCESS;

  // 這就是你看到的 "Not Found" 來源：app 不在 FS 裝置上啟動
  // 所以我們直接 fallback 去找任一個 SimpleFS
  return OpenRootFallbackAnyFs(Root);
}

//
// ---------------- File Helpers ----------------
//
STATIC
EFI_STATUS
GetFileSize(IN EFI_FILE_PROTOCOL *File, OUT UINT64 *FileSize)
{
  EFI_STATUS Status;
  UINTN InfoSize = 0;
  EFI_FILE_INFO *Info = NULL;

  if (File == NULL || FileSize == NULL) return EFI_INVALID_PARAMETER;

  Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) return Status;

  Info = AllocateZeroPool(InfoSize);
  if (Info == NULL) return EFI_OUT_OF_RESOURCES;

  Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (!EFI_ERROR(Status)) {
    *FileSize = Info->FileSize;
  }

  FreePool(Info);
  return Status;
}

STATIC EFI_STATUS OpenFileRead(IN CONST CHAR16 *Name, OUT EFI_FILE_PROTOCOL **File)
{
  if (mRoot == NULL) return EFI_NOT_READY;
  return mRoot->Open(mRoot, File, (CHAR16*)Name, EFI_FILE_MODE_READ, 0);
}

STATIC EFI_STATUS OpenFileCreateWrite(IN CONST CHAR16 *Name, OUT EFI_FILE_PROTOCOL **File)
{
  if (mRoot == NULL) return EFI_NOT_READY;
  return mRoot->Open(mRoot, File, (CHAR16*)Name,
                     EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                     0);
}

//
// ---------------- Actions ----------------
//
STATIC
EFI_STATUS
DoCreateFile(VOID)
{
  EFI_STATUS Status;
  CHAR16 Name[MAX_LINE_CHARS];
  CHAR16 Data[MAX_LINE_CHARS];
  EFI_FILE_PROTOCOL *File = NULL;

  Clear();
  Print(L"[Create file]\n\n");

  Status = GetLine(L"Please input file name: ", Name, MAX_LINE_CHARS);
  if (EFI_ERROR(Status)) { Print(L"(Canceled)\n"); Pause(); return Status; }

  Status = GetLine(L"Please input file data: ", Data, MAX_LINE_CHARS);
  if (EFI_ERROR(Status)) { Print(L"(Canceled)\n"); Pause(); return Status; }

  Status = OpenFileCreateWrite(Name, &File);
  if (EFI_ERROR(Status)) {
    Print(L"Open/Create failed: %r\n", Status);
    Pause();
    return Status;
  }

  // 寫入 ASCII（把 CHAR16 轉成 CHAR8）
  UINTN Len = StrLen(Data);
  CHAR8 *Ascii = AllocateZeroPool(Len + 1);
  if (Ascii == NULL) {
    File->Close(File);
    Print(L"Out of resources.\n");
    Pause();
    return EFI_OUT_OF_RESOURCES;
  }

  for (UINTN i = 0; i < Len; i++) Ascii[i] = (CHAR8)(Data[i] & 0xFF);

  UINTN WriteSize = Len;
  Status = File->Write(File, &WriteSize, Ascii);

  FreePool(Ascii);
  File->Close(File);

  Print(L"\nCreate done: %r\n", Status);
  Pause();
  return Status;
}

STATIC
EFI_STATUS
DoDeleteFile(VOID)
{
  EFI_STATUS Status;
  CHAR16 Name[MAX_LINE_CHARS];
  EFI_FILE_PROTOCOL *File = NULL;

  Clear();
  Print(L"[Delete file]\n\n");

  Status = GetLine(L"Please input file name: ", Name, MAX_LINE_CHARS);
  if (EFI_ERROR(Status)) { Print(L"(Canceled)\n"); Pause(); return Status; }

  Status = mRoot->Open(mRoot, &File, Name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (EFI_ERROR(Status)) {
    Print(L"Open failed: %r\n", Status);
    Pause();
    return Status;
  }

  Status = File->Delete(File); // Delete 會 Close handle
  Print(L"\nDelete done: %r\n", Status);
  Pause();
  return Status;
}

STATIC
EFI_STATUS
DoReadFile(VOID)
{
  EFI_STATUS Status;
  CHAR16 Name[MAX_LINE_CHARS];
  EFI_FILE_PROTOCOL *File = NULL;
  UINT64 Size64 = 0;

  Clear();
  Print(L"[Read file]\n\n");

  Status = GetLine(L"Please input file name: ", Name, MAX_LINE_CHARS);
  if (EFI_ERROR(Status)) { Print(L"(Canceled)\n"); Pause(); return Status; }

  Status = OpenFileRead(Name, &File);
  if (EFI_ERROR(Status)) {
    Print(L"Open failed: %r\n", Status);
    Pause();
    return Status;
  }

  Status = GetFileSize(File, &Size64);
  if (EFI_ERROR(Status)) {
    Print(L"GetInfo failed: %r\n", Status);
    File->Close(File);
    Pause();
    return Status;
  }

  if (Size64 == 0) {
    Print(L"\n[Empty file]\n");
    File->Close(File);
    Pause();
    return EFI_SUCCESS;
  }

  // Demo：避免一次讀超大檔（你要取消限制也行）
  if (Size64 > 1024 * 1024) {
    Print(L"\nFile too large (%lu bytes). Demo limits to 1MB.\n", Size64);
    File->Close(File);
    Pause();
    return EFI_UNSUPPORTED;
  }

  UINTN Size = (UINTN)Size64;
  CHAR8 *Buf = AllocateZeroPool(Size + 1);
  if (Buf == NULL) {
    File->Close(File);
    Print(L"Out of resources.\n");
    Pause();
    return EFI_OUT_OF_RESOURCES;
  }

  Status = File->Read(File, &Size, Buf);
  File->Close(File);

  Print(L"\n---- File Data (ASCII) ----\n");
  Print(L"%a\n", Buf);
  Print(L"---------------------------\n");

  FreePool(Buf);
  Pause();
  return Status;
}

STATIC
EFI_STATUS
DoCopyFile(VOID)
{
  EFI_STATUS Status;
  CHAR16 SrcName[MAX_LINE_CHARS];
  CHAR16 DstName[MAX_LINE_CHARS];
  EFI_FILE_PROTOCOL *Src = NULL, *Dst = NULL;

  Clear();
  Print(L"[Copy file]\n\n");

  Status = GetLine(L"Source file name: ", SrcName, MAX_LINE_CHARS);
  if (EFI_ERROR(Status)) { Print(L"(Canceled)\n"); Pause(); return Status; }

  Status = GetLine(L"Dest file name: ", DstName, MAX_LINE_CHARS);
  if (EFI_ERROR(Status)) { Print(L"(Canceled)\n"); Pause(); return Status; }

  Status = OpenFileRead(SrcName, &Src);
  if (EFI_ERROR(Status)) {
    Print(L"Open source failed: %r\n", Status);
    Pause();
    return Status;
  }

  Status = OpenFileCreateWrite(DstName, &Dst);
  if (EFI_ERROR(Status)) {
    Print(L"Open/Create dest failed: %r\n", Status);
    Src->Close(Src);
    Pause();
    return Status;
  }

  VOID *Chunk = AllocatePool(COPY_CHUNK_SIZE);
  if (Chunk == NULL) {
    Src->Close(Src);
    Dst->Close(Dst);
    Print(L"Out of resources.\n");
    Pause();
    return EFI_OUT_OF_RESOURCES;
  }

  while (TRUE) {
    UINTN ReadSize = COPY_CHUNK_SIZE;
    Status = Src->Read(Src, &ReadSize, Chunk);
    if (EFI_ERROR(Status)) break;
    if (ReadSize == 0) { Status = EFI_SUCCESS; break; }

    UINTN WriteSize = ReadSize;
    Status = Dst->Write(Dst, &WriteSize, Chunk);
    if (EFI_ERROR(Status) || WriteSize != ReadSize) {
      if (!EFI_ERROR(Status)) Status = EFI_DEVICE_ERROR;
      break;
    }
  }

  FreePool(Chunk);
  Src->Close(Src);
  Dst->Close(Dst);

  Print(L"\nCopy done: %r\n", Status);
  Pause();
  return Status;
}

STATIC
EFI_STATUS
DoMergeFile(VOID)
{
  EFI_STATUS Status;
  CHAR16 AName[MAX_LINE_CHARS];
  CHAR16 BName[MAX_LINE_CHARS];
  CHAR16 OutName[MAX_LINE_CHARS];
  EFI_FILE_PROTOCOL *A = NULL, *B = NULL, *Out = NULL;

  Clear();
  Print(L"[Merge two file]\n\n");

  Status = GetLine(L"File A name: ", AName, MAX_LINE_CHARS);
  if (EFI_ERROR(Status)) { Print(L"(Canceled)\n"); Pause(); return Status; }

  Status = GetLine(L"File B name: ", BName, MAX_LINE_CHARS);
  if (EFI_ERROR(Status)) { Print(L"(Canceled)\n"); Pause(); return Status; }

  Status = GetLine(L"Output file name: ", OutName, MAX_LINE_CHARS);
  if (EFI_ERROR(Status)) { Print(L"(Canceled)\n"); Pause(); return Status; }

  Status = OpenFileRead(AName, &A);
  if (EFI_ERROR(Status)) { Print(L"Open A failed: %r\n", Status); Pause(); return Status; }

  Status = OpenFileRead(BName, &B);
  if (EFI_ERROR(Status)) { Print(L"Open B failed: %r\n", Status); A->Close(A); Pause(); return Status; }

  Status = OpenFileCreateWrite(OutName, &Out);
  if (EFI_ERROR(Status)) {
    Print(L"Open/Create output failed: %r\n", Status);
    A->Close(A); B->Close(B);
    Pause();
    return Status;
  }

  VOID *Chunk = AllocatePool(COPY_CHUNK_SIZE);
  if (Chunk == NULL) {
    A->Close(A); B->Close(B); Out->Close(Out);
    Print(L"Out of resources.\n");
    Pause();
    return EFI_OUT_OF_RESOURCES;
  }

  // 把某個檔案完整複製到 Out（Out 目前位置會往後推）
  #define COPY_INTO_OUT(FH) do { \
    while (TRUE) { \
      UINTN RS = COPY_CHUNK_SIZE; \
      EFI_STATUS St = (FH)->Read((FH), &RS, Chunk); \
      if (EFI_ERROR(St)) { Status = St; break; } \
      if (RS == 0) break; \
      UINTN WS = RS; \
      St = Out->Write(Out, &WS, Chunk); \
      if (EFI_ERROR(St) || WS != RS) { Status = EFI_ERROR(St) ? St : EFI_DEVICE_ERROR; break; } \
    } \
  } while(0)

  Status = EFI_SUCCESS;
  COPY_INTO_OUT(A);

  if (!EFI_ERROR(Status)) {
    // 中間加換行（可拿掉）
    CHAR8 NL = '\n';
    UINTN W = 1;
    Out->Write(Out, &W, &NL);

    COPY_INTO_OUT(B);
  }

  #undef COPY_INTO_OUT

  FreePool(Chunk);
  A->Close(A);
  B->Close(B);
  Out->Close(Out);

  Print(L"\nMerge done: %r\n", Status);
  Pause();
  return Status;
}

//
// ---------------- Menu UI ----------------
//
STATIC
VOID
DrawMenu(IN MENU_ITEM Sel)
{
  Clear();

  SetAttrTitle();
  Print(L"File System Utility\n");
  SetAttrNormal();

  CONST CHAR16 *Items[MENU_COUNT] = {
    L"1. Create file",
    L"2. Delete file",
    L"3. Read file",
    L"4. Copy file",
    L"5. Merge two file",
    L"6. Exit"
  };

  for (UINTN i = 0; i < MENU_COUNT; i++) {
    if (i == (UINTN)Sel) SetAttrSelect();
    else SetAttrNormal();
    Print(L"%s\n", Items[i]);
  }

  SetAttrNormal();
  Print(L"\n\nUp: Up   Down: Down   Enter: Select   Esc: Return\n");
}

//
// ---------------- Entry ----------------
//
EFI_STATUS
EFIAPI
UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  EFI_INPUT_KEY Key;
  MENU_ITEM Sel = MENU_CREATE;

  Status = OpenRootRobust(ImageHandle, &mRoot);
  if (EFI_ERROR(Status) || mRoot == NULL) {
    Clear();
    Print(L"OpenCurrentRootByLocateDevicePath failed: %r\n", Status);
    Print(L"Press any key to exit...\n");
    WaitKey();
    ReadKey(&Key);
    return Status;
  }

  while (TRUE) {
    DrawMenu(Sel);

    WaitKey();
    if (EFI_ERROR(ReadKey(&Key))) continue;

    if (Key.ScanCode == SCAN_UP) {
      if (Sel > 0) Sel = (MENU_ITEM)(Sel - 1);
    } else if (Key.ScanCode == SCAN_DOWN) {
      if (Sel + 1 < MENU_COUNT) Sel = (MENU_ITEM)(Sel + 1);
    } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      switch (Sel) {
        case MENU_CREATE: DoCreateFile(); break;
        case MENU_DELETE: DoDeleteFile(); break;
        case MENU_READ:   DoReadFile();   break;
        case MENU_COPY:   DoCopyFile();   break;
        case MENU_MERGE:  DoMergeFile();  break;
        case MENU_EXIT:   goto Exit;
        default: break;
      }
    } else if (Key.ScanCode == SCAN_ESC) {
      // 主畫面 Esc：直接退出（你要改成回到主畫面也行）
      goto Exit;
    }
  }

Exit:
  if (mRoot) mRoot->Close(mRoot);
  return EFI_SUCCESS;
}
