# 周迹（Zhouji Planner）

一个极简的跨平台周计划应用：记录一周任务、勾选完成状态，并可连接兼容 OpenAI Chat Completions 格式的 AI 服务生成计划。目前提供 Windows 桌面版和 Android 版。

## 功能

- 按周一至周日安排任务
- 记录开始时间、时长与任务类别
- 本机保存任务及完成状态
- 自定义 AI 接口地址、模型和 API 密钥
- AI 自动生成周计划，也可复制提示词到其他 AI 应用
- 不包含作者的个人课表或私人数据

## 下载与安装

请在仓库的 **Releases** 页面下载：

- Windows：`周迹通用版-Windows安装包.exe`
- Android：`周迹通用版-Android.apk`

Windows 安装包会把应用与快捷方式安装到当前用户目录。Android 版通过 GitHub 直接分发，不经过应用商店，因此 Android 可能要求你为浏览器或文件管理器开启“允许安装未知应用”。

## AI 接口说明

应用不预设 AI 服务商，也不包含任何 API 密钥。使用者需要自行填写：

- Chat Completions 接口地址
- 模型名称
- API 密钥（如果服务需要）

直接请求接口时，服务端需要允许 Android WebView 跨域访问。如果服务不支持，可以使用“复制规划提示词”功能。

## 数据与隐私

任务、完成状态与 AI 配置保存在应用本机数据中。项目没有广告、统计 SDK 或遥测代码。详情见 [PRIVACY.md](PRIVACY.md)。

## 当前限制

- 每日提醒目前在打开应用时显示，尚未提供 Android 后台系统通知。
- APK 使用轻量 WebView 壳，因此不同 AI 服务的跨域策略可能影响直接连接。

## 从源码构建

源码分别位于 `windows/` 和 `android/`。

Android 构建脚本使用 Python 标准库和 OpenSSL，生成最小 Android APK、资源表与签名：

```powershell
python android/build_apk.py 周迹通用版-Android.apk --icon android/icon.png
```

发布正式更新时，请妥善保存自己的签名私钥；不要将私钥提交到 GitHub。

Windows 版使用 Win32、GDI+ 和 WinHTTP，可用支持 C++17 的 MinGW-w64 构建。图标资源定义位于 `windows/zhouji-share.rc`。

