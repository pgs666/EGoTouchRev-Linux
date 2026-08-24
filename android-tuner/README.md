# EGoTouch Android 调参器

需要 KernelSU/Magisk root 的 Material 3 Android 应用，最低 API 31、目标 API 36（Android 16）。应用读取并写入 Himax 驱动 `algo` sysfs 参数，包含日用/游戏预设，并集成原 `himax-touch-diagnostic` 的 5 秒断触证据采集和硬件重置记录功能。

界面只使用 AndroidX/Google Material 组件；导航图标取自 Google Material Icons 的 `tune` 和 `monitor_heart`，颜色来自 Android 动态系统色资源，随系统深浅色模式切换。

应用通过 AndroidX `EdgeToEdge.enable()` 启用一致的边到边行为；顶部工具栏处理状态栏和显示挖孔 inset，内容处理横屏左右安全区，Material `BottomNavigationView` 处理底部导航栏 inset。

## 构建

需要 JDK 17、Android SDK、Gradle 8.6，以及 `compileSdk 36` 对应的平台文件。项目
不提交 Gradle wrapper、SDK、缓存或预编译 APK。

```sh
export JAVA_HOME=/path/to/jdk17
export ANDROID_HOME=/path/to/android-sdk
gradle :app:assembleDebug
```

产物为 `app/build/outputs/apk/debug/app-debug.apk`。
