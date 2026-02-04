[Defines]
  PLATFORM_NAME                  = FileSystemToolPkg
  PLATFORM_GUID                  = 9cda958f-a3a9-4db3-b324-1ac2116d3a7c
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x0001001A
  OUTPUT_DIRECTORY               = Build/FileSystemToolPkg
  SUPPORTED_ARCHITECTURES        = X64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT

[BuildOptions]
  MSFT:DEBUG_VS2019_X64_CC_FLAGS = /GS- /sdl-
  MSFT:*_*_*_CC_FLAGS = /wd4819
  MSFT:*_*_*_CC_FLAGS = /utf-8
  
[Packages]
  MdePkg/MdePkg.dec
  MdeModulePkg/MdeModulePkg.dec
  EmulatorPkg/EmulatorPkg.dec
  FileSystemToolPkg/FileSystemToolPkg.dec
  
  
[LibraryClasses]
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf

  UefiLib                     |MdePkg/Library/UefiLib/UefiLib.inf
  PrintLib                    |MdePkg/Library/BasePrintLib/BasePrintLib.inf

  UefiBootServicesTableLib    |MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib |MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf

  BaseLib                     |MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib               |MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  MemoryAllocationLib         |MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  DevicePathLib               |MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf

  PcdLib                      |MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  DebugLib                    |MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  RegisterFilterLib           |MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  StackCheckLib               |MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
 


[Components]
  FileSystemToolPkg/Applications/FileSystemTool/FileSystemTool.inf
