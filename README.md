# C 盘清理工具

Windows 下的图形化 C 盘清理工具，先扫描、再勾选、后清理。

后端是 **C++20 + Win32**（`cpp/`），界面是 **Vue 3 + Vite + TypeScript**（`web/`），
通过 **WebView2** 承载，不需要 Rust 工具链。

## 功能

| 页面 | 说明 |
|---|---|
| C盘瘦身 | 按 `rules.json` 统计各清理项占用，勾选后清理 |
| 系统深度瘦身 | 更新缓存、传递优化、组件库、休眠文件、回收站 |
| 碎片清理 | 调用系统 `defrag` 分析和优化，固态盘走 TRIM |
| 隐私清理 | 浏览器历史 / Cookie / 下载记录、最近文件、注册表痕迹 |
| 大文件专清 | 解析 NTFS `$MFT` 快速枚举，大文件移到回收站 |
| 重复文件专清 | 头/中/尾 256KB 采样分组，删除前逐字节完整核对 |
| 微信/QQ 专清 | 按目录语义分类聊天媒体、缓存和日志，支持数据目录搬家 |
| 文件粉碎 | 系统安全随机数覆盖后删除；固态盘上不保证物理层面不可恢复 |
| 清理历史 | 查看每次清理的日志 |

侧栏底部还有「清理历史」「规则更新」「规则回滚」。

## 构建与运行

需要 Visual Studio 2022 Build Tools（MSVC v143）、CMake 4.4+、Node 20+、
WebView2 运行时（Win10 1803+ 一般自带）。

```bash
# 1) 拉依赖：nlohmann/json、SQLite、BLAKE3、WebView2 SDK（按 versions.json 校验 SHA256）
powershell -ExecutionPolicy Bypass -File cpp/tools/fetch_deps.ps1

# 2) 前端
npm --prefix web install
npm --prefix web run build

# 3) 编译（CMake 也会在 web/src 有改动时自动重建前端）
cmake --preset x64-release
cmake --build --preset x64-release
```

一键脚本（自动找 CMake、可选跑测试）：

```powershell
pwsh cpp/tools/build.ps1 -Config release -Test
```

运行、测试、自检：

```bash
cpp/build/x64-release/Release/c-drive-cleaner-cpp.exe
ctest --preset x64-release --output-on-failure
cpp/build/x64-release/Release/c-drive-cleaner-cpp.exe --selftest
```

打包（NSIS 安装包 + 绿色版 ZIP）：

```bash
cd cpp/build/x64-release && cpack -G "NSIS;ZIP"
```

程序启动时会检查管理员权限（读 `$MFT` 需要），不是管理员会用 UAC 重新拉起自己。

## 数据文件

- `rules.json`：清理规则，编译时嵌入二进制，改规则后需要重新编译
- `appdata_path_allowlist.json`：外置/远程规则允许的 AppData 目录模板
- `rules_update.json`：规则更新地址（只接受 https）
- 运行状态都在 `%LOCALAPPDATA%\C盘清理工具\`：`logs\`（清理日志，保留最近 100 份）、
  `mftcache\`（MFT 快照、`hash-cache-v3.db`、重复扫描结果缓存）

## 安全设计

- 永不清理名单（Recovery、System Volume Information 等）、受保护路径、用户文档库、浏览器密码库
- 重解析点（符号链接/目录联接）不跟随，删除按 `NtCreateFile` + 相对句柄进行，不做纯字符串前缀判断
- 大文件和重复副本走回收站；重复文件删除前重新核对哈希、大小和修改时间
- 隐私清理的注册表项删除前用 `reg.exe` 导出 `.reg` 备份，导出失败就跳过
- 文件粉碎有二次确认，系统目录和受保护路径直接拒绝；固态盘有磨损均衡，覆盖只能尽力清除
- 系统瘦身的更新缓存、转储目录逐文件按句柄删除，不跟随重解析点；清理更新缓存前先暂停 wuauserv/BITS
- 微信/QQ 搬家逐文件核对后才删除原目录；中断恢复记录只接受微信/QQ 数据目录及其同级备份

## 目录结构

```
cpp/            C++ 后端
  src/app/      窗口、WebView2 宿主、IPC、前端 shim
  src/core/     扫描、MFT、哈希缓存、清理、隐私、粉碎、系统瘦身、微信/QQ
  src/core/commands/  命令注册与参数解析
  src/util/     UTF-8/UTF-16、路径、JSON、Win32 辅助
  tests/        单元测试（ctest）
web/            Vue 3 前端
```
