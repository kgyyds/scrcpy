# scrcpy server 在 app_process 下如何获取“合法 Context”（Context 获取机制详解）

## 1. 文档目的

本文档详细说明：scrcpy 的 server.jar 在 Android 上通过 `root shell + app_process` 启动后，**如何构造并维持可用（合法）的 `Context`**，从而能正常调用系统服务、PackageManager、ContentResolver、DisplayManager 等能力。

你可以直接把这里的实现迁移到自己的 Java 项目（同样 jar + app_process 运行模型）。

---

## 2. 背景：为什么 app_process 默认没有可用 Context

在 `app_process` 直启模式下，你的 `main()` 不在普通 Android App 进程生命周期中，不会自动获得：

1. `Application`
2. `ActivityThread` 绑定信息
3. 标准 `ContextImpl`

因此需要像 scrcpy 一样，主动做两件事：

1. 构造/填充 `ActivityThread` 关键字段（Workarounds）。
2. 基于 system context 包装出一个 `FakeContext`，并修正 package/opPackage/attribution 等信息。

---

## 3. 总体流程（scrcpy 实际执行顺序）

## 3.1 启动入口先执行 Workarounds

scrcpy 在 `Server` 启动主流程中，先调用：

```java
Workarounds.apply();
```

对应源码：

- `Server.java` 在主启动流程先执行 `Workarounds.apply()`，再继续初始化控制/音视频模块。

这一步是后续 `FakeContext` 可用的前提。

## 3.2 FakeContext 作为全局单例 Context

`scrcpy` 中所有需要 Context 的位置（PackageManager、Clipboard、InputManager、DisplayManager 构造等）统一通过：

```java
FakeContext.get()
```

来获取。

---

## 4. 关键代码与机制拆解（真实摘取 + 说明）

## 4.1 Workarounds 静态初始化：手工创建 ActivityThread

### 4.1.1 真实代码（摘取）

```java
static {
    try {
        // ActivityThread activityThread = new ActivityThread();
        ACTIVITY_THREAD_CLASS = Class.forName("android.app.ActivityThread");
        Constructor<?> activityThreadConstructor = ACTIVITY_THREAD_CLASS.getDeclaredConstructor();
        activityThreadConstructor.setAccessible(true);
        ACTIVITY_THREAD = activityThreadConstructor.newInstance();

        // ActivityThread.sCurrentActivityThread = activityThread;
        Field sCurrentActivityThreadField = ACTIVITY_THREAD_CLASS.getDeclaredField("sCurrentActivityThread");
        sCurrentActivityThreadField.setAccessible(true);
        sCurrentActivityThreadField.set(null, ACTIVITY_THREAD);

        // activityThread.mSystemThread = true;
        Field mSystemThreadField = ACTIVITY_THREAD_CLASS.getDeclaredField("mSystemThread");
        mSystemThreadField.setAccessible(true);
        mSystemThreadField.setBoolean(ACTIVITY_THREAD, true);
    } catch (Exception e) {
        throw new AssertionError(e);
    }
}
```

### 4.1.2 作用说明

1. 让当前进程拥有一个可访问的 `ActivityThread.currentActivityThread()` 上下文基础。
2. 许多系统服务内部会依赖 `ActivityThread` 相关状态；没有这一层，很多调用会崩。
3. 这也是“看起来像系统线程环境”的关键伪装步骤。

## 4.2 Workarounds.apply()：补齐 app 相关运行时信息

### 4.2.1 真实代码（摘取）

```java
public static void apply() {
    if (Build.VERSION.SDK_INT >= AndroidVersions.API_31_ANDROID_12) {
        fillConfigurationController();
    }

    boolean mustFillAppInfo = !Build.BRAND.equalsIgnoreCase("ONYX");

    if (mustFillAppInfo) {
        fillAppInfo();
    }

    fillAppContext();
}
```

### 4.2.2 作用说明

1. **Android 12+**：先填 `ConfigurationController`，避免某些 ROM 在 display 查询时空指针。
2. 填 `mBoundApplication`（`fillAppInfo`）让进程具备基础 app 绑定信息。
3. 填 `mInitialApplication`（`fillAppContext`）让很多依赖 Application/Context 的路径可工作。

## 4.3 获取 system context：FakeContext 的父 Context 来源

### 4.3.1 真实代码（摘取）

```java
static Context getSystemContext() {
    try {
        Method getSystemContextMethod = ACTIVITY_THREAD_CLASS.getDeclaredMethod("getSystemContext");
        return (Context) getSystemContextMethod.invoke(ACTIVITY_THREAD);
    } catch (Throwable throwable) {
        Ln.d("Could not get system context: " + throwable.getMessage());
        return null;
    }
}
```

### 4.3.2 作用说明

1. 通过反射 `ActivityThread.getSystemContext()` 拿到系统级上下文对象。
2. `FakeContext` 正是以这个 system context 为基类包装。

## 4.4 FakeContext 单例：统一向全项目提供可用 Context

### 4.4.1 真实代码（摘取）

```java
public final class FakeContext extends ContextWrapper {

    public static final String PACKAGE_NAME = "com.android.shell";
    public static final int ROOT_UID = 0;

    private static final FakeContext INSTANCE = new FakeContext();

    public static FakeContext get() {
        return INSTANCE;
    }

    private FakeContext() {
        super(Workarounds.getSystemContext());
    }

    @Override
    public String getPackageName() {
        return PACKAGE_NAME;
    }

    @Override
    public String getOpPackageName() {
        return PACKAGE_NAME;
    }
}
```

### 4.4.2 作用说明

1. 全局单例避免不同模块拿到不同形态 Context。
2. `packageName/opPackageName` 固定为 `com.android.shell`，匹配 shell 运行身份。
3. 这对某些系统服务权限判定、AppOps 归因有直接影响。

## 4.5 AttributionSource：Android 12+ 的调用归因修正

### 4.5.1 真实代码（摘取）

```java
@TargetApi(AndroidVersions.API_31_ANDROID_12)
@Override
public AttributionSource getAttributionSource() {
    AttributionSource.Builder builder = new AttributionSource.Builder(Process.SHELL_UID);
    builder.setPackageName(PACKAGE_NAME);
    return builder.build();
}
```

### 4.5.2 作用说明

1. Android 12+ 对调用归因更严格，很多系统 API 会读取 AttributionSource。
2. scrcpy 明确构造 shell 身份归因，降低 ROM 定制导致的调用失败概率。

## 4.6 自定义 ContentResolver：让 settings/content provider 可走通

### 4.6.1 真实代码（摘取）

```java
private final ContentResolver contentResolver = new ContentResolver(this) {
    protected IContentProvider acquireProvider(Context c, String name) {
        return ServiceManager.getActivityManager().getContentProviderExternal(name, new Binder());
    }

    public boolean releaseProvider(IContentProvider icp) {
        return false;
    }

    protected IContentProvider acquireUnstableProvider(Context c, String name) {
        return null;
    }
};

@Override
public ContentResolver getContentResolver() {
    return contentResolver;
}
```

### 4.6.2 作用说明

1. app_process 场景下默认 ContentResolver 链路不一定完整。
2. scrcpy 通过 ActivityManager 外部 provider 获取通道，确保 settings 等 provider 调用可用。

## 4.7 getSystemService 的上下文修补（三星等 ROM 兼容）

### 4.7.1 真实代码（摘取）

```java
@SuppressLint("SoonBlockedPrivateApi")
@Override
public Object getSystemService(String name) {
    Object service = super.getSystemService(name);
    if (service == null) {
        return null;
    }

    if (Context.CLIPBOARD_SERVICE.equals(name) || "semclipboard".equals(name) || Context.ACTIVITY_SERVICE.equals(name)) {
        try {
            Field field = service.getClass().getDeclaredField("mContext");
            field.setAccessible(true);
            field.set(service, this);
        } catch (ReflectiveOperationException e) {
            throw new RuntimeException(e);
        }
    }

    return service;
}
```

### 4.7.2 作用说明

1. 某些 ROM（例如三星）内部 service 持有 `mContext`，若不是“期望形态”会异常。
2. scrcpy 对关键服务强制替换其 `mContext` 为 `FakeContext`，提升兼容性。

---

## 5. 这个 Context 在 scrcpy 中如何被消费

典型用法：

1. `Device.startApp()` 使用 `FakeContext.get().getPackageManager()` 找启动入口。
2. `DisplayManager.createNewVirtualDisplay()` 反射构造 `android.hardware.display.DisplayManager(FakeContext.get())`。
3. `InputManager/Clipboard/CameraManager` 等 wrappers 均通过 `FakeContext.get()` 获取服务。

这说明：**FakeContext 是 server 运行时的统一“合法上下文基座”**。

---

## 6. 迁移到你项目的复用模板

## 6.1 推荐最小迁移文件

1. `Workarounds.java`
2. `FakeContext.java`
3. 依赖的 `ServiceManager`/`ActivityManager` wrapper（至少要支持 provider/service 获取）

## 6.2 你的 main 启动顺序（建议）

```java
public final class Entry {
    public static void main(String... args) {
        // 1) 必须最早执行
        Workarounds.apply();

        // 2) 触发 FakeContext 单例创建
        Context ctx = FakeContext.get();

        // 3) 然后再做显示、启动 app、输入注入等操作
        // ...
    }
}
```

### 说明

1. `Workarounds.apply()` 一定要早于大多数系统服务调用。
2. 若你先访问系统服务再 apply，某些 ROM 可能已经进入错误状态。

---

## 7. 常见失败与排查

## 7.1 `getSystemContext()` 返回 null

排查：

1. 反射是否被系统限制（不同 ROM/SELinux 策略）。
2. 是否在过早阶段执行且类加载不完整。

## 7.2 部分 service 调用崩溃（尤其 clipboard/activity）

排查：

1. 是否保留了 `getSystemService()` 中对 `mContext` 的反射修补。
2. 是否遗漏 `packageName/opPackageName` 覆盖。

## 7.3 provider/settings 调用失败

排查：

1. 是否保留了自定义 `ContentResolver.acquireProvider()`。
2. ActivityManager wrapper 是否实现 `getContentProviderExternal`。

## 7.4 Android 12+ 某些调用权限/归因异常

排查：

1. 是否实现 `getAttributionSource()`。
2. Attribution 是否设置为 shell uid + shell package。

---

## 8. 关键点总结（复用 checklist）

1. [ ] 启动早期执行 `Workarounds.apply()`。
2. [ ] `FakeContext` 继承 `ContextWrapper(systemContext)`。
3. [ ] 固定 `getPackageName()/getOpPackageName()` 为 `com.android.shell`。
4. [ ] Android 12+ 实现 `getAttributionSource()`。
5. [ ] 自定义 `ContentResolver` + provider 获取逻辑。
6. [ ] 对特定 ROM service 执行 `mContext` 修补。
7. [ ] 全局统一使用 `FakeContext.get()`，不要混用其他 Context。

---

## 9. 对照源码位置

1. `server/src/main/java/com/genymobile/scrcpy/Server.java`
   - 启动流程调用 `Workarounds.apply()`
2. `server/src/main/java/com/genymobile/scrcpy/Workarounds.java`
   - `ActivityThread` 构造与字段填充
   - `getSystemContext()`
   - `apply()/fillAppInfo()/fillAppContext()/fillConfigurationController()`
3. `server/src/main/java/com/genymobile/scrcpy/FakeContext.java`
   - `ContextWrapper` 包装
   - package/opPackage/attribution 覆盖
   - ContentResolver 与 getSystemService 兼容逻辑

> 结论：scrcpy 的“合法 Context”不是从标准 App 生命周期拿到的，而是通过 `Workarounds + FakeContext` 主动构造出来的运行时环境。这套方案正是 app_process 模式可复用的关键。
