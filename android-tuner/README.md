# EGoTouch Android 调参器

需要 KernelSU/Magisk root 的 Material 3 Android 应用，最低 API 31、目标 API 36（Android 16）。应用读取并写入 Himax 驱动 `algo` sysfs 参数，包含日用/游戏预设，并集成原 `himax-touch-diagnostic` 的 5 秒断触证据采集和硬件重置记录功能。

界面只使用 AndroidX/Google Material 组件；导航图标取自 Google Material Icons 的 `tune` 和 `monitor_heart`，颜色来自 Android 动态系统色资源，随系统深浅色模式切换。

应用通过 AndroidX `EdgeToEdge.enable()` 启用一致的边到边行为；顶部工具栏处理状态栏和显示挖孔 inset，内容处理横屏左右安全区，Material `BottomNavigationView` 处理底部导航栏 inset。

## 构建

局域网构建服务器的完整连接方式见未提交的 `/home/user/Project/linux-gaokun-buildbot/LOCAL_BUILD_SERVER.md`。当前 Android 工具位置：

- JDK 17：`/home/pgs/gaokun-build/jdk17`
- Android SDK：`/home/pgs/gaokun-build/android-sdk`
- Gradle 8.6：`/home/pgs/gaokun-build/gradle/gradle-8.6/bin/gradle`

```sh
export JAVA_HOME=/home/pgs/gaokun-build/jdk17
export ANDROID_HOME=/home/pgs/gaokun-build/android-sdk
/home/pgs/gaokun-build/gradle/gradle-8.6/bin/gradle :app:assembleDebug
```

产物为 `app/build/outputs/apk/debug/app-debug.apk`。
