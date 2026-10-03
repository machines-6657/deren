# 动态后端(`dynamic_link`)· 现状 / 目标 / 方案

> 本文是**工作账**,不是愿景:每条"实测"都来自本机跑过的命令,没跑的一律标**未验**。
> 生成时间 2026-10-03。相关历史文档见 §7。

---

## §0 一句话

**后端 = 一个 DLL;程序在运行期由 `deren.utility.dynamic_link::load` 解析 3 个 C 入口;`runtime` 变成后端无关(0 个 `Vk*`);`deren.exe` 的导入表最终只剩这 3 个 `deren_*`。不做第二套 —— 没有静态孪生、没有回退路径。**

---

## §1 目标(用户逐条裁决,按裁决原文)

| # | 裁决 | 含义 |
|---|---|---|
| 1 | m04830「让api跟程序本体无关,由dynamic_link获取入口」 | 入口运行期解析,不是链接进来的符号 |
| 2 | 「计划表里导入 3 个函数足够了」「我要动态后端」 | 契约面 = `deren_abi_version` / `deren_make_api_core` / `deren_destroy_api_core`;exe 最终只导入这 3 个 |
| 3 | m05218「vulkan_core 使用了扩展,但是扩展不能被映射成通用的接口」 | 扩展**不进**通用接口:能力位协商 + `vulkan_escape` 交句柄,**不给 `get_*_proc`**(前端自己 `vkGetDeviceProcAddr`,仓库既有做法) |
| 4 | 「不要做回退」「描述符堆都没做回退,为什么这里要保守」 | 依赖的扩展缺失 ⇒ **启动期具名失败**;不留"可选能力/降级/二选一"路径 |
| 5 | 「窗口归应用」 | 应用建窗口/收事件/装回调;后端只从 `native_window` 建 surface,不创建不销毁 |
| 6 | 「搞两套看着玩吗」 | 同一个文件里不允许存在两条并行机制 |

---

## §2 现状(全部实测)

### 2.1 动态链接本身:**0**

| 目标项 | 今天 | 证据 |
|---|---|---|
| 后端是一个 DLL | **否**,`deren_vulkan` = STATIC | `CMakeLists.txt:340 add_library(deren_vulkan STATIC)` |
| 产品代码用到加载器 | **0 处** | 全仓唯一的 `import deren.utility.dynamic_link` 在 `tests/test_dynamic_link.cpp:28` |
| 3 个 C 入口由后端定义 | **0 处** | 全仓无 `std::uint32_t deren_abi_version(...)` 定义 |
| 引擎持有 `shared_ptr<api_core>` | 否 | runtime 仍持 `shared_ptr<core>` |
| build 树里的 `libderen_vulkan.dll` | **陈旧产物**(旧探测构建留下的) | dll 15:16:26 vs `deren.exe` 17:41:40 |

⇒ **主程序还没有"见过"DLL**;它现在仍然在加载期把后端绑在身上。

### 2.2 为它准备好的地基(已落地,未 commit)

| 项 | 状态 | 关键落点 |
|---|---|---|
| 后端半边独立成 target | ✅ | `deren_vulkan`(STATIC),17 个文件从 `vulkancorekit` 搬出;`vulkancorekit PUBLIC deren_vulkan` |
| 契约四分区 | ✅ | `promise/rhi/{rhi,rhi.contract,rhi.core_desc,rhi.extension,rhi.api_core}.cppm`;`abi_version = 4u` |
| 创建参数 = RHI 定义的类型 | ✅ | `deren::promise::rhi::create_info`（原名 `core_create_info`，后端那份孪生结构已删，见 DYNAMIC_LINK_V2.md §10；含 `native_window`）；`main.cpp:511/541` 填充并传边界 |
| 窗口归应用 | ✅ | `main.cpp:502`(RAII 窗口)`/109 glfwInit /115 glfwCreateWindow /133 glfwDestroyWindow /135 glfwTerminate`;后端只绑定 |
| 后端实现契约 | ✅ | `core : deren::promise::rhi::api_core`;`abilities() = to_bits(vulkan_escape)`;`query_extension()` 交 escape;工厂仍 `nullptr`(资源模型未定) |
| 录制面(第二批) | ✅ | `command_list::use(image,from,to)`、`copy_image_to_buffer`、`begin_commands()` 交本帧录制视图;命令缓冲所有权搬进 `core`,帧的 begin/end/提交仍归引擎 |
| 逃生通道 | ✅ | `vulkan_escape = 1u<<5`:4 个 `void*` 句柄 + `enabled_*_extensions()` + `native_command_buffer(command_list&)`(实测映回**同一个** `VkCommandBuffer`);**无 get_*_proc** |
| readback 走契约 | ✅ | `runtime.frames.cppm:3051-3056`(present 屏障之前的帧内录制);`runtime.readback.cppm` 的 `vk*` **6 → 0** |
| 扩展纪律 | ✅ | `VK_EXT_host_image_copy` 硬化成**必需**(扩展+feature+双入口+`pCopySrcLayouts`/`pCopyDstLayouts` 分别要求 GENERAL,缺一具名 panic);上传只走 `vkCopyMemoryToImageEXT`(单一路径) |
| 门 | ✅ | 构建 exit 0;`ctest` 14/14;13 个冻结渲染哈希**逐字节相同**;截图 PNG 逐字节;validation **0**;A5 影子门**逐字段**;verifier 独立复核 **CONFIRMED WITH CAVEATS** |

### 2.3 还欠的账(实测计数)

| 面 | 处数 | 明细 |
|---|---|---|
| `vulkan/runtime/**` 的 `vk*` | **135** | `frames 50`、`probes 37`、`declarations 21`、`cpp 14`、`constructor 13`、`readback **0**` |
| `vulkan/pass/**` 的 `vk*` | **116** | 22 个 pass |
| `vulkan/**` 全部 | **680** | 其余在各资源/RT/管线/Pipeline 面 |
| 3 个 C 入口 / SHARED / loader | 未开始 | — |
| `deren_ext_<ability>_v1` 表 + 能力对象实现 | 未开始 | `device_address`/`descriptor_heap`/`mesh_shader`/`ray_tracing`/`host_image_copy` 的**对象**今天不存在(所以 `abilities()` 只报 `vulkan_escape`) |

---

## §3 方案(顺序固定,每步一个门;不做旁路)

```
① 契约(按 runtime 的需要长大,不提前发明)
        ├─ 已完成批 1:能力协商自洽 + host_image_copy 硬化
        └─ 已完成批 2:录制面 + vulkan_escape + readback 首消费(abi 1→2)
                    ▼
② runtime 的 135 处清零   ← 关键路径,现在在这里
        三类归宿:①只依赖后端状态的 → 下沉进 core
                  ②帧内录制类(屏障/拷贝/堆 push/mesh/RT)→ 走录制面 command_list
                  ③扩展命令与原生句柄 → 走 vulkan_escape,前端自己 vkGetDeviceProcAddr
                    ▼
③ pass 层 116 处 + vulkan/** 其余:vulkan 代码**不搬家**,句柄来自 escape,
        用到的能力写进 required_capabilities();缺失 = 启动期具名失败
                    ▼
④ 翻转(开关,不是实验):deren_vulkan → SHARED(窄导出)
        + 定义 deren_abi_version / deren_make_api_core / deren_destroy_api_core
        + 引擎侧 loader:dynamic_link::load(绝对路径)→ 握手 → deren_make_api_core(POD 创建参数)
        + runtime 持 shared_ptr<api_core>(删除器 = DLL 里的 deren_destroy_api_core)
        ⇒ deren.exe 导入表只剩 3 个 deren_*
```

**为什么翻转放最后**:契约入口交出的对象要能被引擎真正用起来,前提是引擎能通过契约拿到它需要的一切。在 runtime 还抱着 135 处 Vulkan 时换 SHARED,导入表里仍是上百个 mangled 名字 —— 那就是"一个 DLL 放在那儿"。

**已否决的路线(不要再走)**:
- **veneer(阶段 1 兼容方法)**:为 mangled 符号建 111 槽的 naked stub 表。否决理由:契约面是 3 个 C 入口,不是 111 个槽;而且它把"运行期解析"用在错误的符号面上。
- **交换链读回的任一"回退"分支**:实测证伪(见 §6)—— 读回必须录在 present 转换**之前**的帧内命令缓冲里;此外 `VK_EXT_host_image_copy` 在交换链镜像上根本不可用(表面 `supportedUsageFlags` 不含 `HOST_TRANSFER`,`VUID-VkSwapchainCreateInfoKHR-imageUsage-01276`)。
- **tier-1 帧域同步读回**(`frame_image()`+`read_back_frame_image()`):实测 13 条 validation 红(`PRESENT_SRC_KHR` vs 声明 `GENERAL`),已撤回;正确形状是渲染面批次里的帧录制协议。

---

## §4 四条不变式

1. **所有权**:instance / device / queue / swapchain / 分配器 / 描述符堆 / 管线归后端;引擎不创建、不销毁。
2. **引擎侧(含 runtime)不得出现 `Vk` / `vk*`**;要原生句柄只能走 `vulkan_escape`,而它**只允许出现在 pass 层**;任何帧内操作必须录在**本帧命令缓冲**里。
3. **契约演进**:3 个 C 入口永不增长;**S2 之前**往 tier-1 加虚函数允许(每次跳 `abi_version`),**S2 起冻结**,演进只能走新 bit 或 `<ability>_v2`。
4. **无回退 / 无热重载 / 分配不跨界**:依赖的扩展缺失 = 启动期具名失败;DLL 活到进程结束;跨界只有 POD / span / 句柄。

---

## §5 每批次的通用门

```
cmake --build build-release-clang64                      → exit 0
ctest --test-dir build-release-clang64                   → 14/14
pwsh -File scripts\windows\check_render.ps1 -Full -BuildDir build-release-clang64
                                                         → 13 个哈希逐字节相同(不带 -Update)
截图 PNG 逐字节相同                                        → 读回相关批次必查
validation 0 条                                          → 每场景计数
有影子面时逐字段比对(A5)                                  → 只比"有无"不算通过
新增失败路径必须有具名报错样本                              → 不许静默降级
```

冻结的 13 个渲染哈希(deferred `972A31EC5FF55C87`、deferred_taa_fxaa `4021B16AFDB2F43E`、
deferred_ssao_off `BFE3A472FBAB0B5E`、shadow_single `A92C5965316679F3`、unlit `F3C2D7FEFDAD864F`、
transparent_blend `CC7F77F93487AA5E`、sponza `50AF7E46CC1E2A92`、metal_rough_glossy `A1AFBFB61DBFD104`、
glossy_motion `9F31E89BE38B771C`、deformation `723569BA0D03640C`、laevatain_goo_toon `00451C49384D0337`、
laevatain_goo_toon_body `4D9C9B472F37DC6B`、laevatain_old_chain `190EB09D3E9FDCDA`;
`laevatain_no_sidecar` 是**既有红**——缺 `chars/laevatain.glb`,与本工作无关)。

**翻转那一步的额外门**:exe 导入表清点(只剩 3 个 `deren_*`);DLL 导出表清点(窄导出);删掉 DLL ⇒ 具名错误;
`abi_version` 不符 ⇒ 具名错误;probe 树重编。

---

## §6 已知残余与风险(照实)

1. **未落**:3 个 C 入口、SHARED、引擎侧 loader、`deren_ext_<ability>_v1` 表、五个能力对象的具体实现。
2. **未验**:MSVC 树、Debug 树、POSIX;`vulkan_escape` 在 **pass 层**的真实迁移(今天只有引擎自己的帧录制在用);跨 DLL 的 libc++ 版本/分配器规则。
3. **只有代码/编译级证据、无运行期观测**:`frame_image()` 首帧前返 `nullptr`、`native_command_buffer()` 帧外返 `nullptr`(引擎里没有落在那些窗口的调用点)。
4. **实测证伪过的**:tier-1 帧域同步读回(13 条 validation);交换链镜像用 host image copy(表面不带该 usage)。
5. **性能**:host copy **上传**比 staging 慢 1.67×–2.97×;已判定为"形状"(逐 mip 一次调用 + 每 image 两次提交)并排队修正,但**未回退**(用户口径)。
6. **参考资产**:只在 `D:\deren-archive\zmd-archive_20261002_1606.tar.gz`(唯一副本、单盘)。本树用到的那几个成员已按 sha256 取回。
7. **`abi_version` 跳号**:S2 之前在野代价为 0(无发布);S2 起跳号 = 在野 DLL 失配。

---

## §7 相关文档

| 文档 | 作用 |
|---|---|
| `build-release-clang64/deren-ab/rhi/plan_rhi_v4.md` | 总计划(S1–S4、§3.2 逐模块处置表、§3.4 扩展三层)。其 §7"三选一"已被本文 §3 的顺序取代 |
| `build-release-clang64/deren-ab/rhi/dll_compat_entry_plan.md` | 兼容方法草案。其 §2 的 veneer 阶段 1 **已否决**,保留作决策记录 |
| `contract_step2_batch1_report.md` | 批 1 证据(能力硬化、readback 红与根因、13 条 validation 原文) |
| `contract_step2_batch2_spec.md` / `verify_batch2_recording_surface.md` | 批 2 规格 + 独立对抗复核(判词 CONFIRMED WITH CAVEATS) |
| `docs/unified_image_layouts.md` / `docs/host_image_copy.md` | 两条扩展的定位(必需 vs 表面事实) |
| `D:\deren-archive\README.txt` | 归档的成员清单、校验方式、以及"它是唯一副本"的声明 |
