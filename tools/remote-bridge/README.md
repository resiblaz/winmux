# Winmux Remote Bridge + Android 原生 App

直接运行在你自己的 Windows 本机，无需任何云服务器，实现手机 Android 原生 App 实时查看、审批并控制桌面 Winmux 中的 Claude / Codex / AGY 终端。

## 1. 启动本机 Bridge 服务

在 Winmux（或 PowerShell）中执行：

```powershell
pwsh -NoLogo -NoProfile -File .\tools\remote-bridge\Start-WinmuxBridge.ps1
```

启动后会打印：
- `LAN URL`（家里 Wi-Fi 局域网直连地址）
- `Remote URL`（如果本机安装了 Tailscale，会自动识别 `100.x.y.z` 加密远程地址）
- `Pairing Code`（6 位一次性配对码，用完即轮换）
- `Pair URI`（`winmux://pair?...`，可一键粘贴到手机 App）

## 2. 在桌面 Winmux 开启自动化读写权限

在 Winmux 设置（`settings.json` 或设置界面 -> 外观/实验性功能）中打开：
```json
"experimental.workspaceAutomation.allowInput": true,
"experimental.workspaceAutomation.allowContentRead": true
```

## 3. 安装并使用 Android 原生 App

编译或直接安装 APK：
```powershell
pwsh -NoLogo -NoProfile -File .\mobile\android\build-apk.ps1 -Install
```
APK 输出路径：`mobile\android\app\build\outputs\apk\debug\app-debug.apk`。

在手机 App「连接」页粘贴 `Pair URI`（或填入地址与 6 位配对码），点击「完成配对并连接」即可。

## 4. 出门 5G 远程连回本机（免公网 IP / 免云服务器）

1. 电脑与 Android 手机分别安装免费个人版 **Tailscale** 并登录同一账号。
2. 电脑重启 Bridge 后会自动检测到 `100.x.y.z` 地址。
3. 在手机 App「连接」页同时填好局域网地址与 Tailscale 地址：
   - 在家自动走局域网毫秒级直连；
   - 出门切 5G 后后台服务自动无感切换到 Tailscale，熄屏也能收到审批通知并在通知栏直接点「允许一次 / 拒绝」。

## 5. 接入 Agent Hook 决策卡片（示例）

```powershell
# 推送一轮任务完成通知
node .\tools\remote-bridge\hooks\winmux-hook.mjs notify --agent claude --title "Claude 已完成" --body "所有测试通过"

# 弹出权限审批卡片并阻塞等待手机点击「允许一次 / 拒绝」
node .\tools\remote-bridge\hooks\winmux-hook.mjs permission --agent claude --tool Bash --input '{"command":"pwsh -File build.ps1"}' --wait
```
