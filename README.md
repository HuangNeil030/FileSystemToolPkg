# FileSystemToolPkg
cd /d D:\BIOS\MyWorkSpace\edk2

edksetup.bat Rebuild

chcp 65001

set PYTHONUTF8=1

set PYTHONIOENCODING=utf-8

rmdir /s /q Build\FileSystemToolPkg
build -p FileSystemToolPkg\FileSystemToolPkg.dsc -a X64 -t VS2019 -b DEBUG
