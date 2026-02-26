# 在 Java 项目中复用 scrcpy 虚拟显示与应用启动逻辑（app_process / root shell）

## 1. 目标与适用场景

本文档专门用于你这种运行模式：

- Java 工程编译为 `jar`
- 在 Android 设备上通过 `root shell` 使用 `app_process` 启动
- 目标是复用 scrcpy server 的逻辑，实现：
  1. 创建虚拟显示
  2. 将 App 启动到该虚拟显示前台
  3. 将输入事件路由到该虚拟显示

**不讨论**：视频编码、传输、录制。

---

## 2. 先给结论：最小复用闭环

你只需要先打通这 3 步：

1. 创建 `VirtualDisplay`，拿到 `displayId`
2. 启动 App：`NEW_TASK + setLaunchDisplayId(displayId)`
3. 输入注入时绑定 `displayId`

对应 scrcpy 的核心位置：

- 创建显示：`NewDisplayCapture.startNew()`
- 启动 App：`Device.startApp()`
- 输入路由：`Device.injectEvent()` + `Controller.getActionDisplayId()`

---

## 3. 真实代码摘取（来自 scrcpy server）

> 下面代码以“可复用”为目标，优先摘取 scrcpy 现有实现。你的工程若编译方式与 scrcpy server 类似，可直接迁移。

## 3.1 创建虚拟显示（真实摘取：`NewDisplayCapture.startNew()`）

```java
public void startNew(Surface surface) {
    int virtualDisplayId;
    try {
        int flags = VIRTUAL_DISPLAY_FLAG_PUBLIC
                | VIRTUAL_DISPLAY_FLAG_PRESENTATION
                | VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY
                | VIRTUAL_DISPLAY_FLAG_SUPPORTS_TOUCH
                | VIRTUAL_DISPLAY_FLAG_ROTATES_WITH_CONTENT;
        if (vdDestroyContent) {
            flags |= VIRTUAL_DISPLAY_FLAG_DESTROY_CONTENT_ON_REMOVAL;
        }
        if (vdSystemDecorations) {
            flags |= VIRTUAL_DISPLAY_FLAG_SHOULD_SHOW_SYSTEM_DECORATIONS;
        }
        if (Build.VERSION.SDK_INT >= AndroidVersions.API_33_ANDROID_13) {
            flags |= VIRTUAL_DISPLAY_FLAG_TRUSTED
                    | VIRTUAL_DISPLAY_FLAG_OWN_DISPLAY_GROUP
                    | VIRTUAL_DISPLAY_FLAG_ALWAYS_UNLOCKED
                    | VIRTUAL_DISPLAY_FLAG_TOUCH_FEEDBACK_DISABLED;
            if (Build.VERSION.SDK_INT >= AndroidVersions.API_34_ANDROID_14) {
                flags |= VIRTUAL_DISPLAY_FLAG_OWN_FOCUS
                        | VIRTUAL_DISPLAY_FLAG_DEVICE_DISPLAY_GROUP;
            }
        }
        virtualDisplay = ServiceManager.getDisplayManager()
                .createNewVirtualDisplay("scrcpy", displaySize.getWidth(), displaySize.getHeight(), dpi, surface, flags);
        virtualDisplayId = virtualDisplay.getDisplay().getDisplayId();
        Ln.i("New display: " + displaySize.getWidth() + "x" + displaySize.getHeight() + "/" + dpi + " (id=" + virtualDisplayId + ")");

        if (displayImePolicy != -1) {
            ServiceManager.getWindowManager().setDisplayImePolicy(virtualDisplayId, displayImePolicy);
        }

        displaySizeMonitor.start(virtualDisplayId, this::invalidate);
    } catch (Exception e) {
        Ln.e("Could not create display", e);
        throw new AssertionError("Could not create display");
    }
}
```

### 说明（复用重点）

1. flags 是“能不能完整交互”的关键，建议保持 scrcpy 同款分版本追加。
2. `createNewVirtualDisplay(...)` 返回后第一时间拿 `displayId`，后续所有动作都依赖它。
3. `setDisplayImePolicy()` 可选，但在输入法相关场景很实用。

## 3.2 创建显示的底层调用（真实摘取：`wrappers/DisplayManager.createNewVirtualDisplay`）

```java
public VirtualDisplay createNewVirtualDisplay(String name, int width, int height, int dpi, Surface surface, int flags) throws Exception {
    Constructor<android.hardware.display.DisplayManager> ctor = android.hardware.display.DisplayManager.class.getDeclaredConstructor(
            Context.class);
    ctor.setAccessible(true);
    android.hardware.display.DisplayManager dm = ctor.newInstance(FakeContext.get());
    return dm.createVirtualDisplay(name, width, height, dpi, surface, flags);
}
```

### 说明（复用重点）

1. scrcpy 这里用了 `FakeContext` + 反射构造 `DisplayManager`，适配 server 运行环境。
2. 你的 `app_process` 工程可复用这套方式（前提是同样有上下文注入能力）。

## 3.3 启动 App 到指定 display（真实摘取：`Device.startApp()`）

```java
public static void startApp(String packageName, int displayId, boolean forceStop) {
    PackageManager pm = FakeContext.get().getPackageManager();

    Intent launchIntent = getLaunchIntent(pm, packageName);
    if (launchIntent == null) {
        Ln.w("Cannot create launch intent for app " + packageName);
        return;
    }

    launchIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);

    Bundle options = null;
    if (Build.VERSION.SDK_INT >= AndroidVersions.API_26_ANDROID_8_0) {
        ActivityOptions launchOptions = ActivityOptions.makeBasic();
        launchOptions.setLaunchDisplayId(displayId);
        options = launchOptions.toBundle();
    }

    ActivityManager am = ServiceManager.getActivityManager();
    if (forceStop) {
        am.forceStopPackage(packageName);
    }
    am.startActivity(launchIntent, options);
}
```

### 说明（复用重点）

1. 这是最核心启动代码：`NEW_TASK + setLaunchDisplayId(displayId)`。
2. 若 `displayId` 是虚拟显示 id，Activity 会进入该显示的前台栈。
3. `forceStop` 非必需，但冷启动调试时有价值。

## 3.4 等待虚拟显示 id 就绪（真实摘取：`Controller.getStartAppDisplayId()` + `waitDisplayData()`）

```java
private int getStartAppDisplayId() {
    if (displayId != Device.DISPLAY_ID_NONE) {
        return displayId;
    }

    // Mirroring a new virtual display id (using --new-display-id feature)
    try {
        // Wait for at most 1 second until a virtual display id is known
        DisplayData data = waitDisplayData(1000);
        if (data != null) {
            return data.virtualDisplayId;
        }
    } catch (InterruptedException e) {
        // do nothing
    }

    // No display id available
    return Device.DISPLAY_ID_NONE;
}

private DisplayData waitDisplayData(long timeoutMillis) throws InterruptedException {
    long deadline = System.currentTimeMillis() + timeoutMillis;

    synchronized (displayDataAvailable) {
        DisplayData data = displayData.get();
        while (data == null) {
            long timeout = deadline - System.currentTimeMillis();
            if (timeout < 0) {
                return null;
            }
            if (timeout > 0) {
                displayDataAvailable.wait(timeout);
            }
            data = displayData.get();
        }

        return data;
    }
}
```

### 说明（复用重点）

1. 必须先拿到 virtual display id，再启动 App；否则很容易落到主屏。
2. 如果超时，建议明确失败，不要自动 fallback 到主屏（除非你的业务需要）。

## 3.5 输入事件绑定 displayId（真实摘取：`Device.injectEvent()`）

```java
public static boolean injectEvent(InputEvent inputEvent, int displayId, int injectMode) {
    if (!supportsInputEvents(displayId)) {
        throw new AssertionError("Could not inject input event if !supportsInputEvents()");
    }

    if (displayId != 0 && !InputManager.setDisplayId(inputEvent, displayId)) {
        return false;
    }

    return ServiceManager.getInputManager().injectInputEvent(inputEvent, injectMode);
}
```

### 说明（复用重点）

1. `displayId != 0` 时调用 `InputManager.setDisplayId` 是关键步骤。
2. 这一步缺失会出现“画面在虚拟屏，但点击没反应”。

## 3.6 决定输入事件目标 display（真实摘取：`Controller.getActionDisplayId()`）

```java
private int getActionDisplayId() {
    if (displayId != Device.DISPLAY_ID_NONE) {
        // Real screen mirrored, use the source display id
        return displayId;
    }

    // Virtual display created by --new-display, use the virtualDisplayId
    DisplayData data = displayData.get();
    if (data == null) {
        // If no virtual display id is initialized yet, use the main display id
        return 0;
    }

    return data.virtualDisplayId;
}
```

### 说明（复用重点）

1. 这是路由策略模板：真实屏用原始 id；新虚拟屏用回调的 `virtualDisplayId`。
2. 对你项目可以直接复用同样逻辑结构。

---

## 4. 在你项目中的推荐接入方式（app_process）

## 4.1 运行模型建议

1. `Main` 入口通过 `app_process` 启动。
2. 初始化 `FakeContext/ServiceManager`（或你自己的等价封装）。
3. 创建虚拟显示，保存 `virtualDisplayId`。
4. 调用 `Device.startApp(packageName, virtualDisplayId, forceStop)`。
5. 输入通道注入时统一走 `injectEvent(..., virtualDisplayId, ...)`。

## 4.2 伪主流程（可直接改成你的 main）

```java
public final class Entry {
    public static void main(String... args) {
        // 1) 初始化上下文与系统服务封装
        Bootstrap.initForAppProcess();

        // 2) 创建 Surface（来自你自己的渲染链路）
        Surface surface = SurfaceProvider.create();

        // 3) 创建虚拟显示
        VirtualDisplay vd = ServiceManager.getDisplayManager().createNewVirtualDisplay(
                "my-vd", 1920, 1080, 320, surface,
                buildFlagsBySdk()
        );
        int vdId = vd.getDisplay().getDisplayId();

        // 4) 启动 app 到虚拟显示
        Device.startApp("com.example.target", vdId, false);

        // 5) 输入事件路由到 vdId
        // Device.injectEvent(inputEvent, vdId, Device.INJECT_MODE_ASYNC);
    }

    private static int buildFlagsBySdk() {
        int flags = android.hardware.display.DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC
                | android.hardware.display.DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION
                | android.hardware.display.DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY
                | (1 << 6)
                | (1 << 7);
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            flags |= (1 << 10) | (1 << 11) | (1 << 12) | (1 << 13);
        }
        if (android.os.Build.VERSION.SDK_INT >= 34) {
            flags |= (1 << 14) | (1 << 15);
        }
        return flags;
    }
}
```

### 说明

1. 这是“最低可用模板”；你只需替换 `Bootstrap` 和 `SurfaceProvider`。
2. 如果你已迁移 scrcpy wrappers，可直接用同名类以减少改造成本。

---

## 5. API 详细说明（复用时必须理解）

## 5.1 `DisplayManager#createVirtualDisplay(name, width, height, dpi, surface, flags)`

### 参数

1. `name`：显示名称（建议固定前缀，方便 dumpsys 排查）。
2. `width/height`：逻辑分辨率。
3. `dpi`：逻辑密度，影响资源选择。
4. `surface`：显示输出目标。
5. `flags`：行为开关（触摸、焦点、装饰、隔离等）。

### 注意事项

1. flags 必须按 SDK 版本分段设置。
2. 创建后应立刻读取 `displayId` 并缓存。

## 5.2 `ActivityOptions#setLaunchDisplayId(displayId)`

### 作用

决定 Activity 启动到哪个 display。

### 注意事项

1. 前台到虚拟屏的关键 API。
2. displayId 未就绪时不应调用启动。

## 5.3 `Intent.FLAG_ACTIVITY_NEW_TASK`

### 作用

在 `app_process` 场景（非 Activity 上下文）启动 Activity 的必要 flag。

### 注意事项

缺失时常见报错或启动失败。

## 5.4 `InputManager.setDisplayId(event, displayId)` + `injectInputEvent`

### 作用

把输入事件精准投递到目标显示。

### 注意事项

1. 次屏输入在低版本限制更多（scrcpy 用 `supportsInputEvents()` 做门控）。
2. 显示旋转/裁剪时，你还要处理坐标变换。

---

## 6. flags 与参数建议（按 scrcpy 实战）

## 6.1 推荐基础 flags

1. `VIRTUAL_DISPLAY_FLAG_PUBLIC`
2. `VIRTUAL_DISPLAY_FLAG_PRESENTATION`
3. `VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY`
4. `VIRTUAL_DISPLAY_FLAG_SUPPORTS_TOUCH`（内部位 `1<<6`）
5. `VIRTUAL_DISPLAY_FLAG_ROTATES_WITH_CONTENT`（内部位 `1<<7`）

## 6.2 可选 flags

1. `VIRTUAL_DISPLAY_FLAG_DESTROY_CONTENT_ON_REMOVAL`
2. `VIRTUAL_DISPLAY_FLAG_SHOULD_SHOW_SYSTEM_DECORATIONS`

## 6.3 Android 13+ / 14+ 扩展 flags

- Android 13+: `TRUSTED` / `OWN_DISPLAY_GROUP` / `ALWAYS_UNLOCKED` / `TOUCH_FEEDBACK_DISABLED`
- Android 14+: `OWN_FOCUS` / `DEVICE_DISPLAY_GROUP`

---

## 7. 可直接复用 / 需改动

## 7.1 可直接复用

1. `Device.startApp()` 的启动逻辑（`NEW_TASK + setLaunchDisplayId`）。
2. `Controller.waitDisplayData()` 的等待机制。
3. `Device.injectEvent()` 的 display 绑定注入流程。
4. `NewDisplayCapture.startNew()` 的 flags 组装思路。

## 7.2 需改动

1. `FakeContext` 初始化（你项目自己的 app_process 引导逻辑）。
2. `ServiceManager` 封装（可直接拷贝 scrcpy wrappers，再按包名改）。
3. 坐标映射管线（如果你有旋转/裁剪需求）。
4. 日志、异常恢复、进程保活策略。

---

## 8. 常见问题与排查

## 8.1 App 没启动到虚拟显示

检查顺序：

1. `displayId` 是否来自刚创建的 `VirtualDisplay`。
2. 是否调用 `setLaunchDisplayId(displayId)`。
3. 是否加了 `FLAG_ACTIVITY_NEW_TASK`。
4. 是否在 displayId 就绪前就启动了 App。

## 8.2 虚拟显示有画面但点击无效

检查顺序：

1. 注入前是否调用 `InputManager.setDisplayId(event, vdId)`。
2. 设备是否支持对应版本的次屏输入。
3. 触摸坐标是否映射正确。

## 8.3 createVirtualDisplay 失败

检查顺序：

1. flags 是否包含当前系统不支持位。
2. `surface` 是否有效。
3. root/shell 权限与上下文初始化是否完整。

---

## 9. 复用实施清单（建议照单执行）

1. [ ] 迁移 `wrappers.DisplayManager#createNewVirtualDisplay()`。
2. [ ] 迁移 `Device.startApp()`。
3. [ ] 迁移 `Device.injectEvent()`。
4. [ ] 迁移 `Controller.waitDisplayData()/getStartAppDisplayId()`。
5. [ ] 在你的 main 流程里串联：创建显示 -> 等待 id -> 启动 app -> 注入输入。
6. [ ] 加入版本门控与失败重试。

---

## 10. scrcpy 源文件索引（方便你对照复制）

1. `server/src/main/java/com/genymobile/scrcpy/video/NewDisplayCapture.java`
   - `startNew(Surface)`
2. `server/src/main/java/com/genymobile/scrcpy/wrappers/DisplayManager.java`
   - `createNewVirtualDisplay(...)`
3. `server/src/main/java/com/genymobile/scrcpy/device/Device.java`
   - `startApp(...)`
   - `injectEvent(...)`
4. `server/src/main/java/com/genymobile/scrcpy/control/Controller.java`
   - `getStartAppDisplayId()`
   - `waitDisplayData(...)`
   - `getActionDisplayId()`

> 如果你只优先复用两段代码：
> 1) `Device.startApp()`；2) `NewDisplayCapture.startNew()`。
> 这两段决定了“在虚拟显示前台启动并可交互”的核心能力。
