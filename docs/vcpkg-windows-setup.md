# Windows vcpkg 依赖环境配置

本文以 `C:\vcpkg_env` 作为统一的 vcpkg 工作目录示例，用于集中保存：

- vcpkg 工具源码：`C:\vcpkg_env\vcpkg`
- vcpkg manifest 文件：`C:\vcpkg_env\vcpkg.json`
- 编译后的依赖库：`C:\vcpkg_env\vcpkg_installed`

本项目当前默认从 `C:\vcpkg_env\vcpkg_installed\x64-windows` 查找 Boost、spdlog 等依赖。

## 1. 创建 vcpkg 工作目录

打开 PowerShell：

```powershell
New-Item -ItemType Directory -Force C:\vcpkg_env
cd C:\vcpkg_env
```

## 2. 从 GitHub 克隆 vcpkg

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg_env\vcpkg
cd C:\vcpkg_env\vcpkg
.\bootstrap-vcpkg.bat
```

完成后应能看到：

```powershell
C:\vcpkg_env\vcpkg\vcpkg.exe
```

可验证：

```powershell
C:\vcpkg_env\vcpkg\vcpkg.exe version
```

## 3. 通过命令下载 vcpkg.json

如果 `vcpkg.json` 已经在项目根目录，例如：

```text
E:\share\project\video-pipeline\vcpkg.json
```

可以直接复制到 `C:\vcpkg_env`：

```powershell
Copy-Item E:\share\project\video-pipeline\vcpkg.json C:\vcpkg_env\vcpkg.json -Force
```

如果后续把项目推送到了 GitHub，也可以从 raw 地址下载。将下面命令里的 `<owner>`、`<repo>`、`<branch>` 替换为实际仓库信息：

```powershell
Invoke-WebRequest `
  -Uri "https://raw.githubusercontent.com/<owner>/<repo>/<branch>/vcpkg.json" `
  -OutFile "C:\vcpkg_env\vcpkg.json"
```

或者使用 `curl.exe`：

```powershell
curl.exe -L `
  "https://raw.githubusercontent.com/<owner>/<repo>/<branch>/vcpkg.json" `
  -o "C:\vcpkg_env\vcpkg.json"
```

## 4. Windows 环境变量

当前 PowerShell 临时生效：

```powershell
$env:VCPKG_ROOT = "C:\vcpkg_env\vcpkg"
$env:VCPKG_DEFAULT_TRIPLET = "x64-windows"
$env:Path = "$env:VCPKG_ROOT;$env:Path"
```

写入当前用户环境变量，重开终端后生效：

```powershell
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "C:\vcpkg_env\vcpkg", "User")
[Environment]::SetEnvironmentVariable("VCPKG_DEFAULT_TRIPLET", "x64-windows", "User")

$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($userPath -notlike "*C:\vcpkg_env\vcpkg*") {
    [Environment]::SetEnvironmentVariable(
        "Path",
        "$userPath;C:\vcpkg_env\vcpkg",
        "User"
    )
}
```

验证：

```powershell
vcpkg version
```

## 5. 在 CMake 中使用 vcpkg_installed 里的依赖

### 5.1 安装 vcpkg.json 中的依赖

这里将依赖统一安装到 `C:\vcpkg_env\vcpkg_installed`，Release 库会落在：

```text
C:\vcpkg_env\vcpkg_installed\x64-windows
```

执行：

```powershell
C:\vcpkg_env\vcpkg\vcpkg.exe install `
  --triplet x64-windows `
  --manifest-root C:\vcpkg_env `
  --x-install-root C:\vcpkg_env\vcpkg_installed
```

如果想直接使用项目根目录里的 `vcpkg.json`，也可以：

```powershell
C:\vcpkg_env\vcpkg\vcpkg.exe install `
  --triplet x64-windows `
  --manifest-root E:\share\project\video-pipeline `
  --x-install-root C:\vcpkg_env\vcpkg_installed
```

如果你更想把安装目录命名为 `vcpkg_install`，也可以把上面的 `--x-install-root` 改成：

```powershell
--x-install-root C:\vcpkg_env\vcpkg_install
```

对应的 CMake 依赖根目录就是：

```text
C:\vcpkg_env\vcpkg_install\x64-windows
```

### 5.2 方式 A：本项目推荐方式

本项目已经在 `CMakeLists.txt` 中提供了 `VIDEO_PIPELINE_DEPS_ROOT`：

```cmake
set(VIDEO_PIPELINE_DEPS_ROOT "C:/vcpkg_env/vcpkg_installed/x64-windows" CACHE PATH "vcpkg installed prefix")
list(PREPEND CMAKE_PREFIX_PATH "${VIDEO_PIPELINE_DEPS_ROOT}")

find_package(Boost 1.91 CONFIG REQUIRED COMPONENTS json process)
find_package(spdlog CONFIG REQUIRED)
```

配置项目：

```powershell
cmake -S E:\share\project\video-pipeline `
  -B E:\share\project\video-pipeline\build `
  -DVIDEO_PIPELINE_DEPS_ROOT=C:/vcpkg_env/vcpkg_installed/x64-windows `
  -DCMAKE_BUILD_TYPE=Release
```

或者直接使用项目脚本：

```powershell
cd E:\share\project\video-pipeline
powershell -ExecutionPolicy Bypass -File .\build.ps1 build Release `
  -DepsRoot C:\vcpkg_env\vcpkg_installed\x64-windows
```

### 5.3 方式 B：通用 vcpkg toolchain 方式

如果新工程想完全交给 vcpkg toolchain 管理依赖，可以这样配置：

```powershell
cmake -S E:\share\project\video-pipeline `
  -B E:\share\project\video-pipeline\build `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg_env/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DVCPKG_INSTALLED_DIR=C:/vcpkg_env/vcpkg_installed `
  -DCMAKE_BUILD_TYPE=Release
```

CMake 中正常使用 `find_package` 和 imported target：

```cmake
find_package(Boost 1.91 CONFIG REQUIRED COMPONENTS json process)
find_package(spdlog CONFIG REQUIRED)

target_link_libraries(your_target
    PRIVATE
        Boost::headers
        Boost::json
        Boost::process
        spdlog::spdlog
)
```

## 6. 本项目的 Release 输出

本项目 Release 构建时会把运行时 DLL 拷贝到：

```text
E:\share\project\video-pipeline\build\bin
```

来源包括：

- 工程内 FFmpeg：`third_party\ffmpeg-8.1\bin`
- vcpkg 依赖：`C:\vcpkg_env\vcpkg_installed\x64-windows\bin`

因此 Release 产物目录中会包含 Boost、spdlog、fmt、FFmpeg 以及 vcpkg 依赖链上的 DLL。
