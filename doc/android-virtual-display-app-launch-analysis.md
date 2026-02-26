# 虚拟显示复用技术文档（scrcpy server）

## 1. 文档目的

本文档用于直接复用 scrcpy Android server 中“**创建虚拟显示并把 App 启动到该虚拟显示前台**”的实现思路。

聚焦内容：

1. 虚拟显示创建。
2. 指定 display 启动 Activity（前台）。
3. 输入事件按 displayId 路由。
4. 可直接复用代码与改造点。

不包含内容：图像采集、编码、推流。

---

## 2. 复用总览（先看这个）

## 2.1 最小可用链路

1. 创建 `VirtualDisplay` 并拿到 `virtualDisplayId`。
2. 启动目标 App 时设置：
   - `Intent.FLAG_ACTIVITY_NEW_TASK`
   - `ActivityOptions.setLaunchDisplayId(virtualDisplayId)`
3. 输入注入时将事件绑定到 `virtualDisplayId`。

## 2.2 对应 scrcpy 代码位置

- 虚拟显示创建：`NewDisplayCapture.startNew()` + `wrappers.DisplayManager.createNewVirtualDisplay()`。
- 启动 App 到目标 display：`Device.startApp()`。
- 等待 displayId 就绪再启动：`Controller.getStartAppDisplayId()` / `waitDisplayData()`。
- 输入 display 路由：`Controller.getActionDisplayId()` + `Device.injectEvent()`。

---

## 3. 详细流程（可直接照着实现）

## 3.1 步骤 1：创建虚拟显示

1. 决定显示参数：`width`、`height`、`dpi`。
2. 组装虚拟显示 flags。
3. 调用 `DisplayManager#createVirtualDisplay(...)`。
4. 获取 `virtualDisplay.getDisplay().getDisplayId()`。

### 3.1.1 scrcpy 参考实现点

- `NewDisplayCapture.init()`：自动从主屏推导 size/dpi。
- `NewDisplayCapture.startNew()`：拼装 flags 并创建 display。
- `DisplayManager.createNewVirtualDisplay()`：实际调用系统 API。

## 3.2 步骤 2：确保 Surface 绑定正确

1. 创建或准备好渲染 `Surface`。
2. 首次：创建 display 时传入 surface。
3. 重建/重连：调用 `virtualDisplay.setSurface(surface)`。

### 3.2.1 scrcpy 参考实现点

- `NewDisplayCapture.start()`：首次创建或后续 setSurface。

## 3.3 步骤 3：把 App 启动到虚拟显示（前台关键）

1. 获取目标包名 `packageName` 的 launch intent。
2. 添加 `FLAG_ACTIVITY_NEW_TASK`。
3. 构造 `ActivityOptions` 并调用 `setLaunchDisplayId(virtualDisplayId)`。
4. `startActivity(intent, options)`。

### 3.3.1 scrcpy 参考实现点

- `Device.startApp()`：完整实现。
- `Controller.getStartAppDisplayId()`：避免 displayId 尚未可用。

## 3.4 步骤 4：输入事件路由到虚拟显示

1. 维护“当前操作 displayId”。
2. 触摸/鼠标/滚轮等事件注入前设置 displayId。
3. 只有 displayId 正确，虚拟显示上的前台 App 才能真实响应。

### 3.4.1 scrcpy 参考实现点

- `Controller.getActionDisplayId()`。
- `Device.injectEvent()`（设置 displayId + injectInputEvent）。

---

## 4. 可复用代码（重点）

> 下面代码是“可落地”的复用模板；你可直接复制后替换权限、日志、异常处理。

## 4.1 代码段 A：创建虚拟显示（可直接复用，按版本裁剪 flags）

```java
import android.content.Context;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.os.Build;
import android.view.Surface;

public final class VirtualDisplayHelper {

    private VirtualDisplayHelper() {}

    /**
     * 创建虚拟显示并返回实例。
     *
     * @param context 上下文（System/Shell 场景通常来自系统进程或受控上下文）
     * @param name 虚拟显示名称
     * @param width 逻辑宽度
     * @param height 逻辑高度
     * @param dpi 逻辑密度
     * @param surface 输出 Surface
     */
    public static VirtualDisplay createVirtualDisplay(
            Context context,
            String name,
            int width,
            int height,
            int dpi,
            Surface surface
    ) {
        DisplayManager dm = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
        if (dm == null) {
            throw new IllegalStateException("DisplayManager is null");
        }

        // 基础 flags：scrcpy 常用组合
        int flags = DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC
                | DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION
                | DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY
                | (1 << 6)  // VIRTUAL_DISPLAY_FLAG_SUPPORTS_TOUCH（内部位）
                | (1 << 7); // VIRTUAL_DISPLAY_FLAG_ROTATES_WITH_CONTENT（内部位）

        // Android 13+
        if (Build.VERSION.SDK_INT >= 33) {
            flags |= (1 << 10) // VIRTUAL_DISPLAY_FLAG_TRUSTED
                    | (1 << 11) // VIRTUAL_DISPLAY_FLAG_OWN_DISPLAY_GROUP
                    | (1 << 12) // VIRTUAL_DISPLAY_FLAG_ALWAYS_UNLOCKED
                    | (1 << 13); // VIRTUAL_DISPLAY_FLAG_TOUCH_FEEDBACK_DISABLED
        }

        // Android 14+
        if (Build.VERSION.SDK_INT >= 34) {
            flags |= (1 << 14) // VIRTUAL_DISPLAY_FLAG_OWN_FOCUS
                    | (1 << 15); // VIRTUAL_DISPLAY_FLAG_DEVICE_DISPLAY_GROUP
        }

        VirtualDisplay vd = dm.createVirtualDisplay(name, width, height, dpi, surface, flags);
        if (vd == null || vd.getDisplay() == null) {
            throw new IllegalStateException("Failed to create virtual display");
        }
        return vd;
    }
}
```

### 4.1.1 代码说明与注意事项

1. `OWN_CONTENT_ONLY`：建议保留，避免显示内容串扰。
2. `(1<<6)`, `(1<<7)` 等是 internal flag 位；建议封装成常量并按系统版本判断。
3. 低权限普通三方应用环境下，部分 flag 或行为可能被系统限制。
4. 创建失败后要做重试和资源回收（`VirtualDisplay.release()`）。

## 4.2 代码段 B：启动 App 到虚拟显示前台（可直接复用）

```java
import android.app.ActivityOptions;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;

public final class AppLaunchOnDisplayHelper {

    private AppLaunchOnDisplayHelper() {}

    /**
     * 将 App 启动到指定 displayId。
     *
     * @param context Context
     * @param packageName 目标包名
     * @param displayId 目标显示 ID（虚拟显示ID）
     */
    public static void launchToDisplay(Context context, String packageName, int displayId) {
        PackageManager pm = context.getPackageManager();
        Intent launchIntent = pm.getLaunchIntentForPackage(packageName);
        if (launchIntent == null) {
            throw new IllegalArgumentException("No launch intent for package: " + packageName);
        }

        // 非 Activity 上下文启动必须带 NEW_TASK
        launchIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);

        Bundle options = null;
        if (Build.VERSION.SDK_INT >= 26) {
            ActivityOptions ao = ActivityOptions.makeBasic();
            // 核心 API：让 Activity 在目标 display 前台打开
            ao.setLaunchDisplayId(displayId);
            options = ao.toBundle();
        }

        context.startActivity(launchIntent, options);
    }
}
```

### 4.2.1 代码说明与注意事项

1. **这是前台启动到虚拟显示的核心实现**，关键在 `setLaunchDisplayId(displayId)`。
2. 若 displayId 还没准备好，App 可能落到主屏；先等待虚拟显示创建完成。
3. 若业务要求冷启动，可在启动前执行 force-stop（需要系统权限）。

## 4.3 代码段 C：等待 displayId 就绪再启动（可直接复用）

```java
public final class DisplayIdHolder {
    private final Object lock = new Object();
    private Integer virtualDisplayId;

    public void onVirtualDisplayCreated(int displayId) {
        synchronized (lock) {
            virtualDisplayId = displayId;
            lock.notifyAll();
        }
    }

    public int waitDisplayId(long timeoutMs) throws InterruptedException {
        long end = System.currentTimeMillis() + timeoutMs;
        synchronized (lock) {
            while (virtualDisplayId == null) {
                long wait = end - System.currentTimeMillis();
                if (wait <= 0) {
                    throw new IllegalStateException("Timeout waiting virtual display id");
                }
                lock.wait(wait);
            }
            return virtualDisplayId;
        }
    }
}
```

### 4.3.1 代码说明与注意事项

1. 避免“先启动 App、后创建 Display”的竞态。
2. scrcpy 对应实现是 `waitDisplayData(1000)`。
3. 建议超时后明确报错，不要静默回退主屏。

## 4.4 代码段 D：输入事件按 displayId 注入（需改动：权限相关）

```java
import android.view.InputEvent;

public final class InputRouteHelper {

    private InputRouteHelper() {}

    /**
     * 伪代码：将输入事件注入到指定 display。
     * 需要系统权限/隐藏 API 能力。
     */
    public static boolean injectToDisplay(InputEvent event, int targetDisplayId) {
        if (targetDisplayId != 0) {
            // 对应 scrcpy: InputManager.setDisplayId(event, displayId)
            HiddenInputApi.setDisplayId(event, targetDisplayId);
        }
        return HiddenInputApi.injectInputEvent(event);
    }
}
```

### 4.4.1 代码说明与注意事项

1. 普通三方应用无法直接做全局输入注入。
2. 只要 displayId 设置错，就会出现“界面在虚拟屏，点击无效”。
3. 触摸事件还需做坐标变换（旋转/裁剪场景）。

---

## 5. 关键 API 详细说明

## 5.1 `DisplayManager#createVirtualDisplay`

### 5.1.1 作用

创建逻辑显示并返回 `VirtualDisplay` 对象。

### 5.1.2 关键参数

1. `name`：显示名字，用于调试识别。
2. `width/height`：逻辑分辨率。
3. `dpi`：逻辑密度，会影响布局与资源选择。
4. `surface`：显示输出目标。
5. `flags`：显示行为策略（触摸、焦点、装饰等）。

### 5.1.3 复用注意

- flags 必须做版本门控。
- display 生命周期要与 surface 生命周期绑定管理。

## 5.2 `VirtualDisplay#getDisplay#getDisplayId`

### 5.2.1 作用

获取系统分配的目标 displayId，后续用于启动 Activity 与注入输入。

### 5.2.2 复用注意

- displayId 是整个链路的关键主键；建议全局只维护一份“当前有效值”。

## 5.3 `Intent.FLAG_ACTIVITY_NEW_TASK`

### 5.3.1 作用

在非 Activity 上下文中启动 Activity 必备。

### 5.3.2 复用注意

- 缺少此 flag 常见表现是启动异常或行为不稳定。

## 5.4 `ActivityOptions#setLaunchDisplayId`

### 5.4.1 作用

指定 Activity 在哪个 display 打开。

### 5.4.2 复用注意

- 这是“前台到虚拟显示”最关键 API。
- displayId 不可用时不要盲启。

## 5.5 输入注入 API（隐藏能力封装）

### 5.5.1 作用

将输入事件打到目标 display。

### 5.5.2 复用注意

- 通常需要系统权限或 shell/system 执行环境。
- 需要明确键鼠事件与触摸事件的目标 display 策略。

---

## 6. 标志位（flags）复用建议

## 6.1 推荐默认组合

1. `PUBLIC`
2. `PRESENTATION`
3. `OWN_CONTENT_ONLY`
4. `SUPPORTS_TOUCH`（内部位）
5. `ROTATES_WITH_CONTENT`（内部位）

## 6.2 按版本追加

1. Android 13+：`TRUSTED`、`OWN_DISPLAY_GROUP`、`ALWAYS_UNLOCKED`、`TOUCH_FEEDBACK_DISABLED`
2. Android 14+：`OWN_FOCUS`、`DEVICE_DISPLAY_GROUP`

## 6.3 可选开关

1. `DESTROY_CONTENT_ON_REMOVAL`：短生命周期工作区建议开。
2. `SHOULD_SHOW_SYSTEM_DECORATIONS`：需要系统栏时再开。

---

## 7. 可直接复用 / 需改动清单

## 7.1 可直接复用

1. `NEW_TASK + setLaunchDisplayId + startActivity` 启动模型。
2. “等待 displayId 就绪再启动 App”的同步模型。
3. “按 displayId 路由输入事件”的控制模型。

## 7.2 需改动

1. 隐藏 API / 反射封装（`wrappers.DisplayManager`、输入注入层）。
2. flags 策略（不同 ROM/业务需要不同组合）。
3. 坐标映射（你若没有 scrcpy 的映射管线，需要自建变换）。
4. 错误处理策略（重试、回退、告警上报）。

---

## 8. 兼容性与风险

## 8.1 Android 版本

1. Android 8.0+：`setLaunchDisplayId` 可用。
2. Android 10+：次屏输入支持更完整。
3. Android 13/14+：新增 virtual display flags。

## 8.2 ROM 差异风险

1. 厂商对焦点、系统装饰、输入策略可能有改动。
2. 建议建立“机型白名单 + 能力探测 + 降级策略”。

## 8.3 常见失败现象与排查

1. **App 跑到主屏**：通常是 displayId 未就绪或 `setLaunchDisplayId` 未生效。
2. **虚拟屏显示正常但无法点击**：输入事件 displayId 未绑定或坐标映射错误。
3. **创建显示失败**：flag 不兼容、权限不足或 surface 生命周期异常。

---

## 9. scrcpy 源码映射索引（复用时快速定位）

1. `server/src/main/java/com/genymobile/scrcpy/video/NewDisplayCapture.java`
   - `init()`
   - `prepare()`
   - `startNew(Surface)`
   - `start(Surface)`
2. `server/src/main/java/com/genymobile/scrcpy/device/Device.java`
   - `startApp(...)`
   - `injectEvent(...)`
3. `server/src/main/java/com/genymobile/scrcpy/control/Controller.java`
   - `onNewVirtualDisplay(...)`
   - `getStartAppDisplayId()`
   - `waitDisplayData(...)`
   - `getActionDisplayId()`
4. `server/src/main/java/com/genymobile/scrcpy/wrappers/DisplayManager.java`
   - `createNewVirtualDisplay(...)`

---

## 10. 一页式落地清单（实施用）

1. [ ] 封装 `createVirtualDisplay()`，完成 flags 版本门控。
2. [ ] 建立 `DisplayIdHolder`，确保启动前已拿到 displayId。
3. [ ] 封装 `launchToDisplay(packageName, displayId)`。
4. [ ] 封装输入注入层，强制所有事件带目标 displayId。
5. [ ] 增加异常处理：创建失败、启动失败、display 失效重建。
6. [ ] 在目标机型做前台验证：可见、可点、可回退。

> 如果你只复用一件事：优先复用 `setLaunchDisplayId(displayId)` + “等待 displayId 就绪”这两步，这是“App 前台跑在虚拟显示”的决定性链路。
