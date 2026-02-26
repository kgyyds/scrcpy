# scrcpy Android Server 虚拟显示与应用启动逻辑审查

## 引言

本文档面向希望在 **Android 底层 Java 项目中复用 scrcpy server 端虚拟显示能力** 的开发者，目标是系统分析 `server/` 目录中与“创建虚拟显示 + 在目标显示启动应用 + 输入路由”相关的核心实现。重点围绕以下类展开：

- `NewDisplayCapture`
- `Device`
- `Controller`
- `wrappers/DisplayManager`

文档只关注以下主题：

1. 虚拟显示创建与参数控制。
2. Surface 绑定与显示尺寸/坐标变换。
3. 应用如何启动到虚拟显示（强调前台运行，不是后台任务）。
4. 输入事件如何路由到正确 display。
5. 这些逻辑在你自己的 Java 项目中的可复用方式。

> 说明：本文不涉及图像采集链路细节（编码/传输/录制），仅讨论“显示创建与应用启动控制”。

---

## 背景知识

### 虚拟显示（Virtual Display）是什么

虚拟显示是 Android 提供的一种“逻辑显示输出目标”，由系统分配独立的 `displayId`。应用窗口可以被启动到该 display 上，输入事件也可按 `displayId` 注入。

在 scrcpy 的 `--new-display` 路径中，核心是调用：

- `DisplayManager.createVirtualDisplay(...)` 创建新的 display。
- `ActivityOptions.setLaunchDisplayId(displayId)` 指定应用启动目标显示。

这样可形成“物理主屏 + 虚拟屏”的并行显示模型。

### 关键 Android API（本文关注）

- `android.hardware.display.DisplayManager#createVirtualDisplay`
- `android.hardware.display.VirtualDisplay`
- `android.app.ActivityOptions#setLaunchDisplayId`
- `Intent.FLAG_ACTIVITY_NEW_TASK`
- `InputManager` 注入事件时绑定 displayId（通过系统隐藏接口适配）

### 前台渲染 vs 后台运行（本场景）

在这里，“前台运行应用”强调的是：

1. Activity 真正被启动并附着到目标虚拟显示（有窗口可见、可交互）。
2. 输入事件被发往该虚拟显示对应窗口。

而“后台”通常指进程存活但 Activity 不在前台可见栈，或服务在后台执行。scrcpy 的实现目标是前者：**将可见 Activity 启动到指定虚拟 display 并可交互**。

---

## 流程分析（按执行链路）

## 1) 虚拟显示创建流程

1. 解析 `NewDisplay` 配置（是否显式指定分辨率、dpi）。
2. 若未显式指定，从主显示读取 `DisplayInfo`，推导虚拟显示尺寸与 dpi。
3. 构建虚拟显示 flags（公开、演示、仅自身内容、触摸支持、随内容旋转等）。
4. 调用 `DisplayManager.createNewVirtualDisplay(...)`，得到 `VirtualDisplay` 实例和系统分配的 `displayId`。
5. 可选设置 IME policy（`WindowManager#setDisplayImePolicy`）。
6. 启动 `DisplaySizeMonitor` 监听虚拟显示尺寸变化并触发重算。

涉及方法：

- `NewDisplayCapture.init()`
- `NewDisplayCapture.prepare()`
- `NewDisplayCapture.startNew(Surface)`
- `wrappers.DisplayManager.createNewVirtualDisplay(...)`

## 2) Surface 绑定与尺寸/坐标变换流程

1. `prepare()` 根据 crop/rotation/angle/maxSize 计算输出尺寸。
2. 分离两套变换：
   - `eventTransform`：输入坐标映射。
   - `displayTransform`：显示旋转与过滤后纹理变换。
3. `start(surface)` 中若存在 `displayTransform`，先通过 OpenGL 中间层处理后再绑定 Surface。
4. 初次创建调用 `startNew(surface)`；后续重建只 `virtualDisplay.setSurface(surface)`。
5. 创建完成后，通过 `VirtualDisplayListener` 回调给 `Controller` 新 displayId 与 `PositionMapper`。

涉及方法：

- `NewDisplayCapture.prepare()`
- `NewDisplayCapture.start(Surface)`
- `Controller.onNewVirtualDisplay(...)`

## 3) 应用启动到虚拟显示流程（重点）

1. 控制层收到“启动应用”命令后，异步执行 `startApp(name)`。
2. 按包名或名称解析目标应用，获取 `DeviceApp`。
3. 调用 `getStartAppDisplayId()`：
   - 若是镜像真实显示，直接用已有 `displayId`。
   - 若是新建虚拟显示（`DISPLAY_ID_NONE`），等待 `onNewVirtualDisplay()` 回传真实虚拟 `displayId`。
4. `Device.startApp(...)` 中：
   - `launchIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)`。
   - `ActivityOptions.makeBasic().setLaunchDisplayId(displayId)`。
   - 通过 `ActivityManager.startActivity(intent, options)` 启动。
5. 应用 Activity 会在该 display 的前台窗口栈中创建并显示。

涉及方法：

- `Controller.startAppAsync(...)`
- `Controller.startApp(...)`
- `Controller.getStartAppDisplayId()`
- `Device.startApp(...)`

## 4) 输入事件路由流程

1. `Controller` 区分“动作类事件”目标 display：
   - 若是普通镜像：用源 `displayId`。
   - 若是新虚拟显示：用回调得到的 `virtualDisplayId`。
2. 触摸/鼠标坐标事件使用 `PositionMapper` 和 `eventTransform` 映射到目标显示坐标。
3. `Device.injectEvent(...)` 调用前，会对输入事件设置 displayId（非主屏时）。
4. 最终调用 `InputManager.injectInputEvent(...)` 注入系统。

涉及方法：

- `Controller.getActionDisplayId()`
- `Controller.injectTouch(...)` / `injectScroll(...)` / `injectKeyEvent(...)`
- `Device.injectEvent(...)`

## 5) 前后台处理要点

1. 应用启动时使用 `NEW_TASK + setLaunchDisplayId`，确保 Activity 在目标 display 任务栈内创建。
2. `forceStop` 选项用于冷启动，可减少旧任务干扰。
3. 对虚拟显示模式，scrcpy 仍保持对主物理屏电源控制的保守策略，避免误将“虚拟显示前台应用”与“物理屏熄灭状态”混淆。

---

## 关键类与方法解析

### `NewDisplayCapture`

#### 职责

- 计算新显示参数（size/dpi）。
- 创建并维护 `VirtualDisplay`。
- 建立显示变换与输入映射基础。

#### 关键方法

- `init()`：在自动模式下读取主显示尺寸与 dpi，作为推导基准。
- `prepare()`：应用 crop/rotation/angle/maxSize，得到视频尺寸和事件逆变换。
- `startNew(Surface)`：拼装 flags 并创建全新虚拟显示。
- `start(Surface)`：绑定 Surface，首次创建或重绑。

### `Device`

#### 职责

- 提供“设备动作”统一静态入口：输入注入、显示电源、应用启动等。

#### 关键方法

- `startApp(String packageName, int displayId, boolean forceStop)`：核心应用启动逻辑。
- `injectEvent(InputEvent, int displayId, int injectMode)`：统一输入注入与 display 绑定。
- `supportsInputEvents(int displayId)`：按系统版本判断次屏输入可行性。

### `Controller`

#### 职责

- 处理控制通道命令，协调虚拟显示创建后的 displayId 分发、应用启动与输入路由。

#### 关键方法

- `onNewVirtualDisplay(int virtualDisplayId, PositionMapper positionMapper)`：接收虚拟 display 元数据。
- `getActionDisplayId()`：确定当前事件注入 display。
- `getStartAppDisplayId()`：在新显示模式下等待并拿到 displayId。
- `startApp(String name)`：应用查找 + 启动总控。

### `wrappers.DisplayManager`

#### 职责

- 封装系统隐藏 API / 反射调用，稳定地提供显示相关能力。

#### 关键方法

- `createNewVirtualDisplay(...)`：使用 `FakeContext` 构造 `DisplayManager` 并创建虚拟显示。
- `getDisplayInfo(int displayId)`：读取逻辑显示信息（尺寸、旋转、dpi、uniqueId）。
- `registerDisplayListener(...)`：监听 display 变化，辅助动态更新。

---

## 标志位与参数解释

### 虚拟显示 flags（`startNew()`）

#### `VIRTUAL_DISPLAY_FLAG_PUBLIC`

- 作用：使该虚拟显示对系统可见，允许常规显示路由。
- 注意：与私有显示相比，系统组件可更完整感知该 display。

#### `VIRTUAL_DISPLAY_FLAG_PRESENTATION`

- 作用：标记为演示型显示，适用于将内容投送至非主屏场景。

#### `VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY`

- 作用：限制显示内容来源，避免无关内容混入。
- 复用建议：多显示隔离场景建议开启。

#### `VIRTUAL_DISPLAY_FLAG_SUPPORTS_TOUCH`

- 作用：声明显示支持触摸输入路径。
- 注意：仅声明能力，仍需输入注入正确设置 displayId。

#### `VIRTUAL_DISPLAY_FLAG_ROTATES_WITH_CONTENT`

- 作用：显示方向可跟随内容旋转。

#### `VIRTUAL_DISPLAY_FLAG_DESTROY_CONTENT_ON_REMOVAL`

- 作用：移除显示时销毁其内容。
- 适用：短生命周期虚拟工作区，避免残留。

#### `VIRTUAL_DISPLAY_FLAG_SHOULD_SHOW_SYSTEM_DECORATIONS`

- 作用：允许系统装饰（状态栏/导航栏等）出现在虚拟显示。
- 注意：是否生效依赖 ROM 与系统策略。

#### Android 13+ 附加 flags

- `TRUSTED` / `OWN_DISPLAY_GROUP` / `ALWAYS_UNLOCKED` / `TOUCH_FEEDBACK_DISABLED`
- 作用：增强显示信任级别、分组与交互行为控制。

#### Android 14+ 附加 flags

- `OWN_FOCUS` / `DEVICE_DISPLAY_GROUP`
- 作用：焦点管理与 display group 行为更精细。

### Activity 启动参数

#### `Intent.FLAG_ACTIVITY_NEW_TASK`

- 作用：从非 Activity 上下文启动 Activity 的必要 flag。
- 在 server 进程（shell/app_process）场景中通常必须设置。

#### `ActivityOptions.setLaunchDisplayId(displayId)`

- 作用：明确指定 Activity 要启动到哪个显示。
- 这是“前台运行在虚拟显示”最核心参数。

#### `forceStop`（scrcpy 逻辑参数）

- 作用：启动前先 `forceStopPackage`，实现近似冷启动。
- 注意：会清空应用前台态与部分内存状态。

---

## 复用方法建议

## 可直接复用

### 1) 应用按 displayId 前台启动（核心）

- 可直接复用：`Device.startApp()` 的组合思路：`NEW_TASK + setLaunchDisplayId + startActivity`。
- 适用前提：你拥有足够权限（shell/system 或等效能力）调用对应 API。

### 2) 输入事件 display 路由框架

- 可直接复用：`Controller.getActionDisplayId()` + `Device.injectEvent()` 这一“先确定目标 display，再注入”的架构。
- 好处：主屏镜像与新建虚拟显示可共用一套事件通道。

### 3) 等待虚拟 display 就绪再启动应用

- 可直接复用：`waitDisplayData(timeout)` 的同步策略。
- 好处：避免应用先启动导致落到主屏或 displayId 未知。

## 需改动

### 1) `wrappers.DisplayManager` 反射封装

- 需改动：scrcpy 依赖 `FakeContext`、内部封装和隐藏 API 调用习惯。
- 在你的项目中应替换为你自己的 Context/权限模型，并增加 ROM 兼容兜底。

### 2) 虚拟显示 flags 组合

- 需改动：不同业务对系统装饰、安全策略、焦点行为要求不同。
- 建议按 Android 版本与设备类型（车机/平板/手机）做策略表。

### 3) 坐标映射/旋转处理

- 需改动：如果你不走 scrcpy 的 `VideoFilter + PositionMapper` 管线，需要自建映射矩阵。

### 4) 应用查找逻辑

- 需改动：scrcpy 包含“按名模糊查找 + 包名查找”CLI 语义，你的 App 内可能只需固定包名启动。

---

## Android 版本兼容性

### Android 8.0+

- `ActivityOptions.setLaunchDisplayId()` 可用，是多显示定向启动关键。

### Android 10+

- 次屏输入事件注入支持更完整；scrcpy 也按版本判断 `supportsInputEvents()`。

### Android 13/14+

- 虚拟显示新增 flags，scrcpy 做了分版本追加。
- 建议在复用时保留版本门控，避免旧系统传入未知 flags。

### Android 15

- scrcpy 对 display power 新接口持保守态度（代码中明确默认关闭新方案），提示你在新系统行为上要做真机验证。

### ROM 差异

- 厂商 ROM（如某些 Android 14 机型）在显示/电源行为上可能不一致。
- 复用时应增加机型黑名单或 capability 探测。

---

## 核心示例代码（可用于 Java 项目改造）

> 下面代码是基于 scrcpy 逻辑提炼的“关键路径片段”，用于说明“创建虚拟显示 + 前台启动应用 + 输入路由”的最小组合。

### 示例 1：创建虚拟显示并绑定 Surface

```java
// 作用：创建一个可交互的虚拟显示，返回 VirtualDisplay（含 displayId）
// 参数说明：
// - name: 虚拟显示名称
// - width/height/dpi: 目标逻辑分辨率与密度
// - surface: 渲染输出目标（可由你自己的渲染链路提供）
// 注意：flags 需按 Android 版本动态裁剪，避免低版本不识别
int flags = DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC
        | DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION
        | DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY
        | (1 << 6) // SUPPORTS_TOUCH（内部常量，部分版本需自定义）
        | (1 << 7); // ROTATES_WITH_CONTENT

if (Build.VERSION.SDK_INT >= 33) {
    flags |= (1 << 10)  // TRUSTED
          |  (1 << 11)  // OWN_DISPLAY_GROUP
          |  (1 << 12)  // ALWAYS_UNLOCKED
          |  (1 << 13); // TOUCH_FEEDBACK_DISABLED
}
if (Build.VERSION.SDK_INT >= 34) {
    flags |= (1 << 14)  // OWN_FOCUS
          |  (1 << 15); // DEVICE_DISPLAY_GROUP
}

DisplayManager dm = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
VirtualDisplay vd = dm.createVirtualDisplay(
        "MyVirtualDisplay",
        width,
        height,
        dpi,
        surface,
        flags
);

int virtualDisplayId = vd.getDisplay().getDisplayId();
// 后续用于 setLaunchDisplayId() / 输入事件路由
```

#### 注释说明

- `OWN_CONTENT_ONLY` 可减少内容污染，适合你要“独立工作区”的场景。
- `SUPPORTS_TOUCH` 只声明能力，真正可点击要配合输入注入到 `virtualDisplayId`。
- 如果你的项目运行在普通三方应用权限下，某些 flags/行为可能受限。

### 示例 2：将应用前台启动到指定虚拟显示

```java
// 作用：把目标应用 Activity 启动到 virtualDisplayId 对应显示
// 关键点：NEW_TASK + setLaunchDisplayId
PackageManager pm = context.getPackageManager();
Intent launchIntent = pm.getLaunchIntentForPackage(packageName);
if (launchIntent == null) {
    throw new IllegalStateException("No launch intent for package: " + packageName);
}

launchIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);

Bundle options = null;
if (Build.VERSION.SDK_INT >= 26) {
    ActivityOptions activityOptions = ActivityOptions.makeBasic();
    activityOptions.setLaunchDisplayId(virtualDisplayId);
    options = activityOptions.toBundle();
}

context.startActivity(launchIntent, options);
```

#### 注释说明

- `setLaunchDisplayId` 决定 Activity 进入哪个 display 的任务栈，是“前台到虚拟显示”的关键。
- 若你在 service/shell 环境启动，`FLAG_ACTIVITY_NEW_TASK` 通常必需。
- 若启动时机早于虚拟显示完成创建，可能会落回主屏；建议先等待 displayId 就绪。

### 示例 3：输入事件按 displayId 注入（伪代码）

```java
// 作用：将输入事件明确投递到目标显示
// 注意：需要系统级能力；普通应用通常不能直接调用隐藏注入接口
boolean injectToDisplay(InputEvent event, int targetDisplayId) {
    if (targetDisplayId != 0) {
        // 伪代码：给 event 绑定 displayId（scrcpy 通过 InputManager 封装处理）
        HiddenInputApi.setDisplayId(event, targetDisplayId);
    }
    return HiddenInputApi.injectInputEvent(event);
}
```

#### 注释说明

- displayId 路由错误会导致“看得到窗口但点不到”。
- 对触摸坐标要做旋转/裁剪后的映射，避免触点偏移。

---

## 实施建议（在你自己的 Java 项目中）

## 推荐落地顺序

1. 先打通“虚拟显示创建 + 获取 displayId”。
2. 再打通“固定包名应用启动到该 displayId”。
3. 最后接入输入注入与坐标映射。

## 工程化注意事项

1. 增加 display 生命周期管理（创建失败重试、释放回收）。
2. 为不同 API Level 维护 flags 白名单。
3. 为 ROM 差异建立兼容层（特别是焦点、IME、系统装饰）。
4. 应用启动和 display 创建采用串行状态机，避免竞态。

## 快速判断“是否前台成功”

- 现象级：虚拟显示可见应用窗口并可响应点击。
- 系统级：任务栈/窗口归属 displayId 与预期一致。
- 输入级：触摸事件日志中的目标 displayId 与虚拟显示一致。

---

## 结论

scrcpy 的实现给出了一个成熟范式：

1. 用分版本 flags 创建“可交互的独立虚拟显示”。
2. 用 `ActivityOptions.setLaunchDisplayId()` 将应用定向到该显示前台。
3. 用 displayId 感知的输入注入链路保证交互闭环。

对于 Android 底层 Java 项目，这三步正是“在虚拟显示前台运行应用”的最小闭环。你可以直接复用其启动与路由思想，再按项目权限模型、ROM 兼容要求做必要改造。
