# FileSystemToolPkg

---

# FileSystemTool – UEFI File I/O 函數使用筆記（README）

## 1. 專案目標與流程總覽

### 1.1 功能需求

* Create file：建立檔案並寫入使用者輸入內容
* Delete file：刪除檔案
* Read file：讀檔並顯示內容
* Copy file：複製檔案
* Merge two file：合併兩個檔案內容到新檔

### 1.2 典型流程（你寫的工具就是這條）

1. 取得 `EFI_FILE_PROTOCOL* Root`（Root 目錄）
2. `Root->Open()` 開檔（Read / Write / Create）
3. `File->Read()` / `File->Write()` / `File->Delete()` / `File->GetInfo()`
4. `File->Close()`（Delete 例外：Delete 會自動 Close）

---

## 2. 關鍵 Protocol 與資料結構

### 2.1 `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`

**目的：** 取得 Volume（檔案系統）的 Root 目錄 handle
**最重要的函數：** `OpenVolume()`

```c
EFI_STATUS
(EFIAPI *EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME)(
  IN  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
  OUT EFI_FILE_PROTOCOL               **Root
);
```

* `This`：SimpleFS 協定實例
* `Root`：輸出 Root Directory 的 `EFI_FILE_PROTOCOL*`

**常見錯誤碼**

* `EFI_NOT_FOUND`：此 handle 沒有 SimpleFS（常見：app 從 FV 啟動）
* `EFI_DEVICE_ERROR`：媒體/驅動錯誤
* `EFI_ACCESS_DENIED`：存取權限問題

---

### 2.2 `EFI_FILE_PROTOCOL`

**目的：** 對檔案/資料夾做一切操作（Open/Read/Write/Delete/GetInfo/Close）
**核心函數：**

* `Open()`
* `Read()`
* `Write()`
* `Delete()`
* `GetInfo()`
* `Close()`

---

### 2.3 `EFI_FILE_INFO` + `gEfiFileInfoGuid`

**目的：** 取得檔案大小、屬性、時間戳等
**方法：** `File->GetInfo(File, &gEfiFileInfoGuid, ...)`

---

## 3. Root 取得（你遇到 Not Found 的重點）

### 3.1 為什麼 `LocateDevicePath` 常失敗（Not Found）

當你的 `.efi` 不是從 `FS0:\xxx.efi` 啟動，而是從：

* Firmware Volume（FV）
* 特殊 loader
* 或某些裝置不提供 SimpleFS

就會導致「用 DevicePath 找 SimpleFS」回 `EFI_NOT_FOUND`。

### 3.2 正確穩定解法：兩段式 Root 取得（推薦）

**策略：**

1. 先用 `LoadedImage->DeviceHandle` 嘗試拿 SimpleFS（最符合“目前裝置”）
2. 若失敗就 fallback：掃描第一個 SimpleFS

---

## 4. Boot Services 常用函數（定位 Protocol/Handle）

### 4.1 `gBS->HandleProtocol()`

**用途：** 從一個 `EFI_HANDLE` 上取出某個 Protocol 的介面指標

```c
EFI_STATUS
(EFIAPI *EFI_HANDLE_PROTOCOL)(
  IN  EFI_HANDLE Handle,
  IN  EFI_GUID   *Protocol,
  OUT VOID       **Interface
);
```

* `Handle`：你要查的 handle
* `Protocol`：例如 `&gEfiSimpleFileSystemProtocolGuid`
* `Interface`：輸出 protocol instance 指標（例如 SimpleFS 指標）

**常見錯誤碼**

* `EFI_NOT_FOUND` / `EFI_UNSUPPORTED`：該 handle 沒有這個 protocol

---

### 4.2 `gBS->LocateHandleBuffer()`

**用途：** 找出所有支援某 Protocol 的 handles（用來 fallback）

```c
EFI_STATUS
LocateHandleBuffer(
  IN  EFI_LOCATE_SEARCH_TYPE SearchType,
  IN  EFI_GUID               *Protocol OPTIONAL,
  IN  VOID                   *SearchKey OPTIONAL,
  OUT UINTN                  *NoHandles,
  OUT EFI_HANDLE             **Buffer
);
```

**常見用法：**

* `SearchType = ByProtocol`
* `Protocol = &gEfiSimpleFileSystemProtocolGuid`

**常見錯誤碼**

* `EFI_NOT_FOUND`：找不到任何符合的 handle（系統可能沒有可用 FS）

---

### 4.3 `EFI_LOADED_IMAGE_PROTOCOL`

**用途：** 找到本 App 的載入來源裝置 `DeviceHandle`

```c
Status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&Loaded);
DeviceHandle = Loaded->DeviceHandle;
```

---

## 5. EFI_FILE_PROTOCOL 常用函數（檔案操作）

### 5.1 `Root->Open()`：開檔 / 建檔

```c
EFI_STATUS
(EFIAPI *EFI_FILE_OPEN)(
  IN  EFI_FILE_PROTOCOL  *This,
  OUT EFI_FILE_PROTOCOL  **NewHandle,
  IN  CHAR16             *FileName,
  IN  UINT64             OpenMode,
  IN  UINT64             Attributes
);
```

**重要參數**

* `This`：通常是 `Root`
* `NewHandle`：輸出檔案 handle
* `FileName`：檔名（`CHAR16*`）
* `OpenMode`：

  * `EFI_FILE_MODE_READ`
  * `EFI_FILE_MODE_WRITE`
  * `EFI_FILE_MODE_CREATE`（不存在就建立）
* `Attributes`：建立檔案時可用，通常 0 即可

**常見錯誤碼**

* `EFI_NOT_FOUND`：檔案不存在（且沒 CREATE）
* `EFI_ACCESS_DENIED`：權限不足
* `EFI_WRITE_PROTECTED`：Volume 只讀
* `EFI_VOLUME_FULL`：空間不足

---

### 5.2 `File->Read()`：讀檔

```c
EFI_STATUS
(EFIAPI *EFI_FILE_READ)(
  IN     EFI_FILE_PROTOCOL *This,
  IN OUT UINTN             *BufferSize,
  OUT    VOID              *Buffer
);
```

**重點**

* `BufferSize` 是「in/out」

  * In：你希望最多讀多少 bytes
  * Out：實際讀到多少 bytes
* 讀到 EOF 時：`BufferSize` 會變成 0（這是 chunk copy 的終止條件）

**常見錯誤碼**

* `EFI_DEVICE_ERROR`：裝置讀取錯誤

---

### 5.3 `File->Write()`：寫檔

```c
EFI_STATUS
(EFIAPI *EFI_FILE_WRITE)(
  IN     EFI_FILE_PROTOCOL *This,
  IN OUT UINTN             *BufferSize,
  IN     VOID              *Buffer
);
```

**重點**

* `BufferSize` 也是 in/out

  * Out 可能小於 In（表示沒寫完整），通常要視為錯誤處理

**常見錯誤碼**

* `EFI_WRITE_PROTECTED` / `EFI_ACCESS_DENIED`
* `EFI_VOLUME_FULL`
* `EFI_DEVICE_ERROR`

---

### 5.4 `File->Delete()`：刪檔（重要陷阱）

```c
EFI_STATUS
(EFIAPI *EFI_FILE_DELETE)(
  IN EFI_FILE_PROTOCOL *This
);
```

**超重要：**

* `Delete()` 成功後 **會自動 Close 這個 handle**
* 所以 Delete 後 **不要再 File->Close(File)**

**常見錯誤碼**

* `EFI_ACCESS_DENIED`
* `EFI_WRITE_PROTECTED`

---

### 5.5 `File->Close()`：關檔

```c
EFI_STATUS
(EFIAPI *EFI_FILE_CLOSE)(
  IN EFI_FILE_PROTOCOL *This
);
```

---

### 5.6 `File->GetInfo()`：拿檔案大小（兩段式呼叫）

```c
Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, NULL);
// 預期回 EFI_BUFFER_TOO_SMALL

Info = AllocateZeroPool(InfoSize);
Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, Info);
Size = Info->FileSize;
```

**常見錯誤碼**

* `EFI_BUFFER_TOO_SMALL`：第一階段正常現象
* `EFI_DEVICE_ERROR`

---

## 6. UI/鍵盤輸入常用 Protocol（做你那個選單必備）

### 6.1 `gST->ConIn->ReadKeyStroke()` + `WaitForKey`

* `WaitForKey` 是 event，可用 `gBS->WaitForEvent()` 等待
* `ReadKeyStroke()` 取出 `EFI_INPUT_KEY`

**方向鍵判斷**

* `Key.ScanCode == SCAN_UP`
* `Key.ScanCode == SCAN_DOWN`
* `Key.ScanCode == SCAN_ESC`

**Enter 判斷**

* `Key.UnicodeChar == CHAR_CARRIAGE_RETURN`

---

## 7. 功能實作的「套公式」

### 7.1 Create file（建檔+寫入）

1. `Root->Open(... READ|WRITE|CREATE ...)`
2. `File->Write(...)`
3. `File->Close(...)`

### 7.2 Read file（讀檔+顯示）

1. `Root->Open(... READ ...)`
2. `File->GetInfo()` 拿 `FileSize`
3. `AllocatePool(FileSize+1)`
4. `File->Read(...)`
5. `Print(...)`
6. `File->Close(...)`

### 7.3 Copy file（chunk copy）

1. 開 Src（READ）
2. 開 Dst（READ|WRITE|CREATE）
3. while：

   * `Read(Size=4096)` → 若 0 表示 EOF
   * `Write(同樣大小)`
4. Close

### 7.4 Merge（copy A + newline + copy B）

1. 開 A/B（READ）
2. 開 Out（CREATE）
3. copy A → write '\n' → copy B
4. Close

---

## 8. 常見 Debug Checklist（出問題先查這些）

* `EFI_NOT_FOUND`（開 Root）：

  * 你的 `.efi` 不是從 FS 啟動 → 用 fallback 掃 SimpleFS
* `EFI_WRITE_PROTECTED`：

  * FS0 是只讀（某些平台/介面）
* `EFI_ACCESS_DENIED`：

  * 路徑/目錄權限、檔案屬性、或實作限制
* `EFI_VOLUME_FULL`：

  * 空間不足
* `EFI_DEVICE_ERROR`：

  * 媒體或 driver 出錯（USB/虛擬磁碟）

---

## 9. 建議延伸（下一步你可以升級的點）

* 支援選擇 FS0/FS1（列出每個 volume label）
* Read 檔案內容分頁顯示/滾動
* 支援 UTF-16 檔案內容（Create/Read 都用 Unicode）
* Copy/Merge 支援超大檔（不限制 1MB）
---
cd /d D:\BIOS\MyWorkSpace\edk2

edksetup.bat Rebuild

chcp 65001

set PYTHONUTF8=1

set PYTHONIOENCODING=utf-8

rmdir /s /q Build\FileSystemToolPkg

build -p FileSystemToolPkg\FileSystemToolPkg.dsc -a X64 -t VS2019 -b DEBUG
