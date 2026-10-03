# 动态后端执行方案 v2（修订提案）

> 2026-10-03 评审修订见 [DYNAMIC_LINK_V3.md](DYNAMIC_LINK_V3.md)。本文保留历史记录；“EXE 导入三个入口”、PRIVATE切断静态库传递依赖、计数棘轮即翻转证明等口径以 V3 为准。本次验证另见 DYNAMIC_LINK_V3_RESULT.md。

> 本文是 [`DYNAMIC_LINK.md`](DYNAMIC_LINK.md) 的**执行层修订提案**，不替换它：§1 的六条裁决、§4 的四条不变式、§0 的目标原样有效。
> 本文只改四件事——**度量轴、步骤顺序、归宿分类、门清单**。
> 每条"实测"都给了命令；没跑的标**未验**。基线时间 2026-10-03，`build-release-clang64`。

---

## §0 一句话

**翻转的门槛不是 680 处 `vk*`，而是"引擎引用了多少个后端定义的 C++ 符号"：今天 78 个符号 / 227 处引用 / 31 个 obj。**
这条轴线一旦换对，关键路径就从"清 680 处拼写"变成"清 78 个符号"——其中 **52 个是资源/所有权面**（真正要设计的东西，不是文本替换），**翻转可以提前到资源面契约化之后**，而不是等到 `vulkan/**` 全部清零。

已有工具：`scripts/check_backend_boundary.py` + `scripts/backend_boundary_baseline.json`（棘轮门，与 13 个渲染哈希同一套纪律）。

---

## §1 与现方案的四点差异（其余全部保留）

| # | 维度 | `DYNAMIC_LINK.md` | 本文 | 依据 |
|---|---|---|---|---|
| 1 | **度量轴** | 680 处 `vk*`（§2.3） | **78 符号 / 227 处引用** | §2 实测；`vk*` 另列为"解耦债"，只作趋势 |
| 2 | **翻转时机** | ④ 最后（②③ 之后） | **②a 资源面之后即可翻转**；②b/③ 可放翻转后 | `plan_rhi_v4.md:1247` 原意"把'接口设计错'和'DLL 边界坏'分开归因" |
| 3 | **归宿分类** | 三类 | **四类**：补"被拥有的资源 → 契约工厂 + desc" | §4 实测：52/78 是所有权句柄，escape 只发 `void*`，无法承载所有权（违反不变式 1） |
| 4 | **ABI 面** | 隐含要建 `deren_ext_<ability>_v1` C 表 | **建议删除该层** | §5；待裁决 1 |

另外两处**恢复**（v4 有、现方案掉了）：无条件影子门（`plan_rhi_v4.md:923`）、ASan/UBSan+DLL 与 35 个 `.spv` 门（v4 S2 门，现 §5 只剩 13 哈希）。一处**引正**：`DYNAMIC_LINK.md:149` 说"plan §7 三选一"——v4 §7 是 loader 章，三选一在 §6.4。

---

## §2 实测基线（翻转门的定义）

```powershell
$nm='C:\msys64\clang64\bin\llvm-nm.exe'
& $nm --defined-only   --no-demangle build-release-clang64\libderen_vulkan.a    # 18 member / 1149 符号
& $nm --undefined-only --no-demangle build-release-clang64\libvulkancorekit.a   # 77 member
# 交集 = 翻转工作清单
python scripts\check_backend_boundary.py --list
```

| 量 | 值 | 备注 |
|---|---|---|
| **跨边界符号**（`deren_vulkan` 定义 ∩ `vulkancorekit` 未定义） | **78** | 脚本输出 |
| 引用处数 | **227** | 同一符号可被多个 obj 引用 |
| 反方向（`deren_vulkan` → `vulkancorekit`） | **0** | S1-A 的刀口单侧干净 |
| `main.cpp` / `chores` 对象 → `deren_vulkan` | **0** | 应用层是干净的 |
| 跨界的 vtable/typeinfo（`_ZTV`/`_ZTI`） | **0** | `-fno-rtti`，符合预期 |
| 签名里带 `Vk*` 的跨界符号 | 21 / 78 | 这些必须走 escape 或重写签名 |

**按类别（符号）**：native RAII 句柄（`vk_image/buffer/image_view/sampler/pipeline/shader_module/command_buffer`）**30** · `core::core` 成员 **25** · `descriptor_heap` **8** · `init_utils` **6** · `vma_allocator` **6** · `pipeline::make_pipeline` **2** · 模块 initializer **1**。

**按区域（符号×member 对）**：`runtime.*` **115** · 22 个 pass **64** · 其余（pipelines / readback / ray_tracing / acceleration_structure / primitive / filters）**48**。

### 两条独立证据互校

| 方法 | 树 | 结果 |
|---|---|---|
| `nm` 交集（本文） | `build-release-clang64`（17:40） | 78 符号 |
| 导入表（朴素翻转的产物） | `build-release-dllprobe-clang64`（15:02） | 从 `libderen_vulkan.dll` 导入 **110 个名字，其中 75 个 C++ mangled**、33 个 `glfw*`、**0 个 `deren_*`**；另从 `vulkan-1.dll` 导入 72 个 `vk*` |

两种方法在不同树、不同时间量同一件事，得 75 与 78——**互证成立**。同时也说明朴素翻转的后果：exe **加载期**自动导入 DLL（正是 `dynamic_link` 要替掉的绑定），且带进 33 个 `glfw*`（两份 `_glfw` 全局状态）。

### `vk*` 计数（保留为"解耦债"）

规则可复现且与文档一致：`\bvk[A-Z][A-Za-z0-9_]*` **全匹配**（不是行数、不是 `Vk*` 类型名）→ `vulkan/runtime` **135** / `vulkan/pass` **116** / `vulkan/**` **680**，逐文件也对上（frames 50 / probes 37 / declarations 21 / runtime.cpp 14 / constructor 13 / readback 0）。

但它数的是 **token 出现次数**（一行 3 个 `vkCmd*` 记 3），而且其中一大类（经 `vulkan-1.dll` 的屏障/拷贝）**根本不进 `deren_vulkan` 的导入表**。§2.3 的"22 个 pass"在该规则下其实是 **26 个文件**。

> **陷阱**：`build-release-clang64/CMakeFiles/vulkancorekit.dir` 里躺着 18 个 **S1-A 之前的陈旧 obj**（14:56；CMakeLists 15:43 才改）。从目录统计会得出 **130** 而不是 78。**一律用归档量**。

---

## §3 关键路径（新）

```
① 边界可测（已完成：脚本 + 基线 78）
        ▼
①b 边界尖刺（时间盒）——把 v4 §9 #14–#17 提前结掉
        ▼
②a 资源 / 所有权面契约化（52 个符号）      ← 现在在这里，真正的关键路径
        ▼                              ┌── ②b core::core 的 25 个成员 → 已存在的 api_core 虚函数
     ④ 翻转（boundary = 0 是硬门）──────┤
        ▼                              └── ③ pass 面 64 处（借来的走 escape / 拥有的走契约）
③' 收尾：删 ABI 面、冻结
```

**为什么翻转能提前**：翻转只受"引擎引用后端 C++ 符号"约束。78 个里 52 个（资源面）+ 25 个（`core::core`）清掉即可翻转；pass 面剩下的调用点若走 escape（`native_command_buffer` 已就位）与契约录制面（批 2 已交付），**不进 `deren_vulkan` 导入表**。这与"接口设计错 / DLL 边界坏分开归因"是同一件事。

---

## §4 分步方案

### 步 ① 边界可测 —— **已完成**

* 工具：`scripts/check_backend_boundary.py`（实测：正常 exit 0；把基线压到 77 → exit 1 并逐条列出 NEW 符号）。
* 基线：`scripts/backend_boundary_baseline.json`，记录今日 78/227 + 完整符号表；`--update` 是**只降不升**的棘轮。
* 待做：接 CI（先 warn 后 error）；写进 §5 的通用门，**每次提交**跑一次（秒级）。

### 步 ①b 边界尖刺（时间盒 2–3 天）★ 提前结掉已知风险

复用现成资产，不发明第二套机制：`tests/probe_backend.cpp`（同源双编 SHARED+STATIC）+ `build-release-dllprobe-clang64` 树。

把**真的** `deren_vulkan` 以 SHARED 编一次（允许 78 个符号还在——链接器/导入表会直接把它们列出来），再加最小的 `deren_make_api_core` 返回真 `core`，经真 `dynamic_link` 加载，跑一个场景。

必须回答（每条要么**实测**，要么**具名失败**）：

| # | 问题 | 来源 |
|---|---|---|
| 1 | ASan/UBSan 插桩的 exe 加载插桩的 DLL | v4 §9 #14（S2 第一个待测项） |
| 2 | 真 DLL 的 `shared_ptr` 删除器（probe 只验过 `probe_backend`） | v4 §9 #15 |
| 3 | 两个静态 mimalloc 实例、跨实例 free | v4 §9 #16 |
| 4 | 模块 BMI 跨 target 一致性（换编译器/标准库必须同期重建） | v4 §9 #17 |
| 5 | **依赖闭包**：`LoadLibraryExW` 用 `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR\|DEFAULT_DIRS`（`utility/dynamic_link.cppm:19-20,311`），**不搜 `%PATH%`**；真 DLL 会带 `libc++.dll`/`libunwind.dll`/`vulkan-1.dll`/`USER32`——probe DLL 零依赖，这条**未验** | 本文新增 |
| 6 | CMake 是否允许 **SHARED 目标的 `CXX_MODULES` file set** 被 STATIC 消费方消费（probe 是从 STATIC `promise` 导入的） | 本文新增 |

**出口**：一份"边界可行性"结论。若某项不成立 → 回来改契约形状，**而不是继续迁移**。这是本步的全部价值。

### 步 ②a 资源 / 所有权面契约化（关键路径）★

* 目标：清掉 **52** 个符号 = RAII 句柄 30 + `descriptor_heap` 8 + `vma_allocator` 6 + `init_utils` 6 + `pipeline` 2。
* **真正的前置交付物**（不是文本替换）：
  1. 7 个工厂的 desc 定型（`image_desc/sampler_desc/shader_desc/pipeline_desc/swapchain_desc/query_desc/acceleration_structure_desc` 现在还是 incomplete type，`core.api_core.cpp:199-225` 全返回 `nullptr`）；
  2. `image/buffer/view/sampler` 的**用法与屏障模型**（`image_use` 今天只有 `color_attachment`/`transfer_source` 两个值）；
  3. 所有权归属：谁能销毁、在哪销毁（不变式 1）。
* **顺手修掉一个层次违规**：后端里躺着 4 个**引擎专属管线配方**（`make_character_forward_pipeline` / `make_gbuffer_pipeline` / `make_outline_pipeline` / `make_overlay_pipeline`）。契约声明"不许学引擎概念"，这四条必须变成 desc + 引擎侧配方。
* **顺手拆掉一个跨实例 free 入口**：`init_utils::create_host_buffers(..., std::vector<vk_buffer> &, ...)`——mangled 签名里带 `std::allocator`，是 v4 §9 #16 的具体形态。
* **出口**：boundary 计数 78 → ≤26；13 个渲染哈希逐字节；validation 0；A5 影子面逐字段。

### 步 ②b `core::core` 的 25 个成员 → 已存在的虚函数

* 大部分是**改名而非设计**：`present` / `acquire_next_image` / `wait_frame_slot` / `to_next_frame` / `recreate_swap_chain` ↔ 已存在的 `api_core::present` / `frame_begin` / `wait_idle`。
* 需要**新增**虚函数的只有三族：`render_extent`、`set_window_title`、gpu timing（`begin/mark/read_gpu_timings`、`gpu_timing_available`）。按不变式 3，S2 前每次跳 `abi_version`。
* 还要处理 `std::make_shared<core>`（`runtime.constructor.cppm:131,137`）与 `core& vulkan_core`（`runtime.declarations.cppm:172`，26 个调用点）——这是"引擎直接 deref 后端对象"的硬阻塞点。
* **出口**：计数 → ≤ pass 的余量。

### 步 ③ pass 面 64 处（第四类归宿）

* 分类：**借来的**句柄（`vk_image::handle()/valid()` 这类访问器）走 escape；**被拥有的**走契约工厂。
* **必须先补的缺口**：`vulkan_escape` 今天只有 device 级 4 个句柄 + `native_command_buffer(command_list&)` + `enabled_*_extensions`（`rhi.extension.cppm:230-259`）——**没有 `native_image/native_buffer/native_image_view/native_sampler/native_pipeline`**。所以"pass 层句柄来自 escape"目前**不可实现**，二选一：给 escape 补资源级句柄翻译，或按 v4 S3 把 pass 迁到契约（代价见裁决 2）。
* **必须先定**：前端如何取得 `vkGetDeviceProcAddr`（见裁决 3）。
* **出口**：计数 = 0；每个 pass 的 A/B 像素比对（v4 S3 的门）。

### 步 ④ 翻转

* **硬门**：`check_backend_boundary.py` 计数 **= 0**（此时脚本本身就是证明）。
* **CMake**（现方案没列）：
  1. `target_link_libraries(vulkancorekit PUBLIC deren_vulkan)`（`CMakeLists.txt:648`）→ **`PRIVATE`**，BMI 由单独的 INTERFACE target 传递。否则 DLL 的 import lib 进 `vulkancorekit` 的链接接口，exe 加载期自动导入 DLL（§2 已实测到这一后果）。
  2. `deren_vulkan` 生成 `DEREN_API_*` 定义（今天只有 `probe_backend` 有，`:903`）。
  3. **窄导出**：GNU `--export-all-symbols` 会导出 1214 个名字（`dll_compat_entry_plan.md` §6 记录），需 `.def`。
* **glfw 移出 DLL**：`deren_vulkan`(:374) 与 `vulkancorekit`(:626) 都静态链 glfw，朴素翻转实测 33 个 `glfw*` 导入 / 两份 `_glfw` 状态。裁决 5 说"窗口归应用"，但后端今天还在调 `glfwGetFramebufferSize`（`init_utils.cppm:1293`）并持 `GLFWwindow*`（`filters`）。把 framebuffer size 放进 `create_info` / 由应用回传即可摘掉——**同时落地裁决 5**。
* **依赖布局**：exe 不再是单文件，须随 `libc++.dll`；在 `DLL_LOAD_DIR|DEFAULT_DIRS` 下哪些能找到，由步 ①b 第 5 条回答。
* **门**：exe 导入表清点（从 `deren_vulkan` 只导入 3 个 `deren_*`，且**不导入 `vulkan-1.dll`**——这是比"数 mangled 名字"更硬、也更有意义的判据）；DLL 导出表清点；删 DLL ⇒ 具名错误；`abi_version` 不符 ⇒ 具名错误；probe 树重编；MSVC/Debug 树各一遍。

### 步 ③' 收尾

* 删 `deren_ext_<ability>_v1` 层（裁决 1）。
* **把"冻结"与"翻转"解耦**：`abi_version` 进 `deren_make_api_core` 参数、不匹配即拒绝加载（`plan_rhi_v4.md:790`），所以翻转**不需要**先冻结 vtable——冻结的技术约束只在有在野发布时才出现（§6.7 自己写了"无发布 ⇒ 在野代价 0"）。

---

## §5 度量与门（替换/补强 §2.3、§5）

**两个指标，不许混用**：

| 指标 | 含义 | 工具 | 归零时机 |
|---|---|---|---|
| **翻转门** = 跨边界符号数 | 引擎引用后端 C++ 符号 | `scripts/check_backend_boundary.py`（棘轮，每次提交） | 步 ④ 之前 = 0 |
| **解耦债** = `vk*` 出现次数 | 不变式 2 的文本口径 | 现成的正则脚本 | 逐步降，不是翻转门 |

**通用门（每批次）**：

```
cmake --build build-release-clang64                    → exit 0
ctest --test-dir build-release-clang64                 → 14/14
python scripts\check_backend_boundary.py               → 不增长（棘轮）    ← 新增
pwsh -File scripts\windows\check_render.ps1 -Full      → 13 哈希逐字节
35 个 .spv 逐字节                                       ← 恢复（v4 S2）
validation 0 条 / 每场景
A5 影子面逐字段比对（**无条件**，不写"有影子面时"）        ← 恢复（v4:923）
新增失败路径必须有具名报错样本
```

**翻转那一步的额外门**：上文步 ④ 的门清单 + ASan/UBSan+DLL 组合（步 ①b 的结论作为前提）。

---

## §6 需要你裁决的六件事

> 状态：**#4 已关闭**（本次交付：入口签名 + `create_info` 统一，见 §10）；**#5 #6 是 §11 的所有权分析新冒出来的**，属于 ②a 的入口条件。
> 本文的其余部分按"#1 #2 #3 取建议值、#5 #6 待定"的前提继续，改动只落在文档里，未动代码。

| # | 问题 | 选项 | 我的建议 |
|---|---|---|---|
| 1 | `deren_ext_<ability>_v1` C 函数表 | 删 / 留 | **删**。v4 §3.4 记的理由是"完全不依赖跨 DLL 的 C++ vtable ABI"，但 tier-1 `api_core` **本身就是经 C 工厂交出的 C++ 虚表**，这条"不依赖"已经花掉；留下只多出"命名成为 ABI + 版本存两份 + 每能力一个 C 工厂 + §8 一致性门"，而那个门今天过不了。除非有非 C++ 消费者 |
| 2 | pass 面归宿 | 留 escape（现方案，便宜）/ 按 v4 S3 迁契约（1300 处） | **留 escape**，但**在文档里写下代价**：等于放弃 v4 S4（第二后端）——换 DX12 时 22 个 pass 仍要重写 |
| 3 | `vkGetDeviceProcAddr` 谁给 | 引擎自己 `GetModuleHandleW("vulkan-1.dll")`+`GetProcAddress` / escape 补一个 proc 查询 | **前者**（与裁决 3"不给 get_*_proc"一致），但要**先写下来并测**；否则步 ③ 落不了地 |
| 4 | ~~翻转时机~~ **已定** | ②a 之后（本文 / v4 原意）/ ④ 最后（现方案） | **②a 之后**；这是"分开归因"的前提，也是把 v4 §9 #14–#17 从最晚挪到最早的唯一办法 |
| 5 | ~~资源句柄的释放语义~~ **已定（你的裁决）** | 虚析构 + `std::unique_ptr` / 显式虚函数 | **显式 `virtual release()` + 模板 `object_manager<T>`**：已落地，语义是"引用减一，资源可能被复用"，见 §12 |
| 6 | **登记册 / 引用计数 / 内容去重**放哪边 | 留后端（契约增补 retain / 去重语义）/ 搬到引擎（引擎自己做缓存） | **留后端**——已被 #5 的"资源可能被复用"语义确认（后端 `vma_allocator` 就是登记册 + 引用计数 + XXH3 去重）。**剩下的只有内容键的形状**：`image_desc` 带像素字节（今天 `create_texture_2d` 的做法）还是带引擎算好的 128 位摘要 |

---

## §7 代价与未验

* **代价**：步 ①b ~3 天；步 ②a 是整个工作的大头（52 个符号背后是 desc / 用法 / 所有权三件设计，不是替换）；步 ③ 若选 escape 则便宜，选契约则回到 v4 的 1300 处。
* **未验**（照实）：MSVC 树、Debug 树、POSIX；`utility/dynamic_link.cppm` 的 POSIX `dlopen` 分支**从未构建过**；SHARED 目标的 BMI 被 STATIC 消费方消费；真 DLL 的依赖闭包；两个静态 mimalloc 实例。
* **本文所有计数**都来自 `build-release-clang64` 的归档与 `build-release-dllprobe-clang64` 的导入/导出表。**换树必须重跑 `--update`**，否则棘轮会误报。

---

## §8 与现有文档的关系

| 文档 | 关系 |
|---|---|
| [`DYNAMIC_LINK.md`](DYNAMIC_LINK.md) | **不替换**。批准后把本文 §1/§2/§5 并回它的 §2.3/§3/§5；§1 六条裁决、§4 四条不变式不动 |
| `build-release-clang64/deren-ab/rhi/plan_rhi_v4.md` | 本文**恢复**其 S1→S2→S3 的"分开归因"意图（:1247、:1257），把 S2 的入口条件从"接口觉得差不多了"换成 §2 的**符号硬门**；并恢复其门清单（:923、S2 的 .spv 与 sanitizer） |
| `dll_compat_entry_plan.md` | 其 §7 的 veneer 阶段 1 仍未走（已否决）；其 §6 的 `.def` / `--export-all-symbols` 记录沿用 |
| `RHI_NUMBER_RECHECK.md` | 其 raw/call/code 口径（654/418）与本文不冲突：那是**文本**普查，本文是**链接**普查 |

---

## §9 边界尖刺：实测结果（2026-10-03，本机）

**做了什么**（都是产品代码里真实的部分，不是一次性脚本）：

| 新增/改动 | 作用 |
|---|---|
| `vulkan/core/core.entry.cpp` | **真后端**的 3 个 C 入口（S2 的交付物），按 `promise/rhi/backend_entry.hpp` 今天**声明的**签名实现 |
| `tests/spike_backend_boundary.cpp` | 尖刺测试：绝对路径加载 → abi 握手 → 虚假 abi 具名拒绝 →（可选）真 context + 虚调用 + DLL 内删除 |
| `CMakeLists.txt` | `-DDEREN_BACKEND_SPIKE=ON`：把 `deren_vulkan` 变 SHARED（+`DEREN_API_SHARED/BUILD`）并加尖刺目标；默认 OFF，产品树不受影响 |

**构建与运行**：`build-spike-clang64` → `libderen_vulkan.dll`（686,592 B，**导出正好 3 个符号**）+ 尖刺 exe；安全模式 **9 checks / 0 failed / exit 0**。

| # | 问题 | 结果 |
|---|---|---|
| Q5 | 依赖闭包在 `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR\|DEFAULT_DIRS` 下（不搜 `%PATH%`） | ✅ **通过**。DLL 导入 `vulkan-1.dll`、`libc++.dll`、`USER32/GDI32/SHELL32/comdlg32` + UCRT，绝对路径加载成功。**前提是承重的**：`libc++.dll` 能找到是因为 exe 已把它加载进来（同名模块复用），不是这两个 flag 找到的——部署布局仍然必须对 |
| Q6 | CMake 能否让 SHARED 目标的 `CXX_MODULES` file set 被 STATIC 消费方消费 | ✅ **通过**：`vulkancorekit`（真消费方）在 SHARED provider 下正常编译归档（它没有链接步骤，所以 BMI 那一半是它证明的；`deren.exe` 的链接失败是 78 符号的事，不是 BMI 的事） |
| Q4 | 模块 BMI 跨 target / 契约两边各自编译 | ✅ **通过**。DLL 里编译出的 `deren_abi_version()` = 2，与本 exe 编译的 `rhi::abi_version` 相同；虚调用路径在安全模式下已验证到"能解析符号并具名拒绝" |
| Q2 | 真 DLL 的 `shared_ptr` 删除器 | ⚠️ **机制成立，运行期未确认**：删除器符号从 DLL 解析、容忍 `nullptr`、构造/销毁都在 DLL 侧 TU 内——但成功路径要有真 context，被下面的设备路径阻塞 |
| Q1 | ASan/UBSan + DLL | ❌ **两条路都不通，而且比"未验"严重得多**：<br>(a) 保留插桩 → **lld 崩溃**：`LLVM ERROR: Associative COMDAT symbol '.str.5' is not a key for its COMDAT`（Exception `0xC000001D`，clang 22.1.8 `--coff`）；<br>(b) 只给 DLL 加 `-fno-sanitize=all`（probe 的做法）→ 链接失败：`libutility.a` 的对象**是插桩的**，引用 `__asan_handle_no_return` / `__ubsan_*`，而 DLL 链接不注入 runtime。<br>**probe 之所以一直过，是因为它只依赖 `promise`**（纯接口，没有会被插桩的函数体），不依赖 `utility` |
| Q3 | 两个静态 mimalloc 实例 / 跨实例 free | ❌ **在本配置下不成立**：`utility` 引用 `mi_aligned_alloc` / `mi_free_aligned`，但没有任何**被保留**的 section 用到 `better_pmr` → `libmimalloc_vendored.a` 的成员从未被拉入 → DLL 与 exe 里 `mi_*` 符号都是 **0**，也没有 mimalloc DLL 依赖。**风险是条件性的**：DLL 侧代码一旦开始用 `better_pmr`，它就立刻变成活的 |
| Q-export | 窄导出是否需要 `.def`（`dll_compat_entry_plan.md` §6 担心 1214 个名字） | ✅ **不需要**：只要有 `dllexport` 符号，GNU ld 就停止 auto-export。实测导出**正好 3 个、0 个 C++ mangled**（对照：朴素 flip 的 dllprobe 导出 1574 个） |
| Q-device | `--with-device` 成功路径 | ⚠️ **本环境无法完成**：进程 5 分钟只用 0.5s CPU、无主窗口标题地挂住。headless CI 覆盖不了这条 |

### 这次尖刺对方案的改动

1. **结掉 3 项**：Q4（BMI 跨 target）、Q5（依赖闭包）、窄导出（`.def` 的担忧划掉）。
2. **改写 1 项**：Q3 从 v4 §9 #16 的"未验风险"改成**条件下的风险**，并给出触发条件（DLL 侧一旦使用 `better_pmr`）。
3. **暴露一项新代价（必须记账）**：翻转后 **sanitizer job 无法再覆盖产品路径**。`utility` / `promise` / `vstd_lib` 被 DLL 与 exe **同时**静态链接，一个归档不能既插桩又不插桩；而 DLL 侧保留插桩会让 lld 直接崩。所以要么给 DLL 一套不插桩的依赖副本（等于放弃那条路径的 sanitizer 覆盖），要么承认该 job 只覆盖 headless 测试路径。**这是 v4 §9 #14 的真实代价，`DYNAMIC_LINK.md` §5 的门清单里它整个缺席。**
4. **缺口已关闭（本文交付本次改动）**：`deren_make_api_core` 原来只有 `(abi, error*)`，**装不下创建参数**。已按建议形状改成 `deren_make_api_core(std::uint32_t abi, deren::promise::rhi::create_info const* desc, error* out)`，并**把后端的 `vulkan::core_create_info` 与 `to_backend_create_info()` 翻译一起删掉**——契约的创建结构现在是**唯一**一份（见 DYNAMIC_LINK_V2 §10）。`abi_version` 由 2 跳到 **3**（C 入口签名变了），`probe_backend` / `test_dynamic_link` 同步；空 `desc` 具名拒绝为 `error::invalid_argument`。
5. **设备路径不可用于 CI**：`deren_make_api_core` 的成功路径需要真窗口/真设备。要么给后端加一个 headless（无窗口）创建模式，要么把 Q2/Q4 的强形式降级为"手工一次性验证"。

### 复现命令

```powershell
cmake -S . -B build-spike-clang64 -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_C_COMPILER=C:/msys64/clang64/bin/cc.exe -DCMAKE_CXX_COMPILER=C:/msys64/clang64/bin/c++.exe `
      -DCMAKE_MAKE_PROGRAM=C:/msys64/clang64/bin/ninja.exe -DDEREN_BACKEND_SPIKE=ON
cmake --build build-spike-clang64 --target test_backend_boundary_spike
.\build-spike-clang64\test_backend_boundary_spike.exe                  # 安全模式：Q4(弱)/Q5/Q6/握手
.\build-spike-clang64\test_backend_boundary_spike.exe --with-device    # Q2/Q4 强形式（需要显示器+设备）
# Q1：同一套配置再加 ci.yml 的两行 flags，会看到 lld 崩溃 / 未定义 __asan_*
```

> **环境注记（实测）**：本机 `workspace-write` 沙箱下 **ninja 无法完成任何真实构建**——它会以 0 CPU 挂住，尽管产物已经生成（`ninja --version` 正常）。尖刺的构建是在放宽模式下完成的。这条与代码无关，但它意味着"在受限沙箱里跑这个仓库的构建"本身不可行。

---

## §10 创建参数：两份结构合并成 rhi 里唯一的一个 `create_info`

**做了什么**（本次改动）：把后端自己那份 `deren::vulkan::core_create_info` **删掉**，创建参数从此只有契约里的一份——`deren::promise::rhi::create_info`（`promise/rhi/rhi.core_desc.cppm`，原名 `core_create_info`）。因此 `to_backend_create_info()` 这个"翻译"也不存在了：后端的构造函数直接读契约的字段。

| 面 | 改前 | 改后 |
|---|---|---|
| 结构 | `rhi::core_create_info` **+** `vulkan::core_create_info` 两份 | `rhi::create_info` **一份** |
| 转换 | `to_backend_create_info()` 逐字段映射 | **删除**；只剩 ABI 守卫 `sanitize_create_info()`（按 `struct_size` 决定哪些字段可读） |
| 后端构造 | `core(vulkan::core_create_info const&)` + 转发 | `core(rhi::create_info const&)` **唯一** |
| runtime 构造 | 两个（后端类型 + 契约类型） | **一个**：`runtime(rhi::create_info const&)` |
| C 入口 | `deren_make_api_core(abi, error*)`，**装不下创建参数** | `deren_make_api_core(abi, rhi::create_info const* desc, error*)` |
| `abi_version` | 2 | **3**（C 入口签名变了；无虚表位移） |
| 空 `desc` | — | 具名拒绝：`error::invalid_argument`（"没有创建参数"是调用方 bug，不是"要标准 context"；后者写作 `create_info{}`） |

**保留的东西**（不是翻译，是 ABI 守卫）：`struct_size` 的语义一字未改——字段只追加不重排，越界字段保持本 build 的默认值，并在不匹配时记一条日志。后端里 `create_options` 仍按值保存整份结构（`vsync`/`validation_layers`/`window_visible`/尺寸在构造之后还要读），但 **`window_title` 是借来的 `char const*`，只在构造期交给 GLFW**（GLFW 自己复制文本），所以成员上的注释明确写了"永不二次读取"。

**为什么这是 net 简化**：加一个字段从"两处结构 + 一处映射 + 一次 `covers()` 判断"变成"一处结构 + 一次 `covers()` 判断"；反过来说，改前那种安排正好违反本项目自己的裁决 6（同一个文件里不允许两条并行机制）——只不过它跨了两个文件。

**随之更新的门与测试**：
* `tests/probe_backend.cpp`：入口收新签名；把 `create_info.window_width` **回声**进 `frame_begin()` 的 `submit_info.image_index`——这是"描述符真的跨过去了"的唯一可观测量（探针没有设备，无法真正使用其余字段）。
* `tests/test_dynamic_link.cpp`：ABI 钉到 **3**；新增"空 desc ⇒ `invalid_argument`"检查；填一个非默认值描述符，用回声证明它被读到了；DLL 半与静态半走同一套断言。
* `tests/spike_backend_boundary.cpp`：跟着换签名，并在安全模式里加了一条"真后端对空 `create_info` 具名拒绝"的检查（11 checks / 0 failed）。

**实测影响（翻转门）**：78 → **77** 符号 / 227 → **226** 处引用。少掉的那个正是两个 `core` 构造函数合并成一个——`_ZN…core4coreC1ERKNS_7promise3rhi…11create_infoE`。也就是说这次合并**净减**了一处跨边界依赖（两份结构 + 一处映射一起消失）。

**验证（都跑过）**：
* 产品树 `cmake --build build-release-clang64`（全目标，含 `deren.exe`）**exit 0**；`ctest` **14/14**；`clang-format-check` **exit 0**。
* 尖刺树：`deren_abi_version()` = 3 == 本 exe 编译的 3；DLL 导出仍**正好 3 个**（新参数没有破坏窄导出）。
* 边界棘轮：`check_backend_boundary.py` 77/226，基线已 `--update` 到 77。

> **门自身的一个陷阱（本次踩到并已加固）**：只重建 `deren_vulkan` 而不重建 `vulkancorekit`，会让引擎侧仍引用"后端已不再定义"的旧符号 → 这些引用落出交集 → **计数假性下降**（实测会得到 76，而不是这次改动真实的 77）。脚本现在会在"引擎归档比后端归档旧 5 分钟以上"时打印 WARNING，提醒先重建两半再看数字。

---

## §11 步 ②a 的第一刀：所有权面（实测）

**为什么先做这个**：desc 的形状和屏障模型都由"谁拥有、在哪销毁"决定；反过来不成立。所以 ②a 的第一步不是设计接口，而是把所有权事实读清楚。

**资源面的实际构成**（77 个符号里 52 个）：

| 组 | 符号数 | 性质 |
|---|---|---|
| RAII 句柄类型的成员函数（`vk_buffer`/`vk_image`/`vk_image_view`/`vk_sampler`/`vk_command_buffer`/`vk_pipeline` 的 ctor/dtor/reset/handle/valid） | **30** | 引擎"持有后端 RAII 对象"的**后果** |
| `descriptor_heap` | 8 | **操作**，不是所有权 |
| `vma_allocator` | 6 | 创建 + 查询 + 一致性修补 |
| `init_utils` | 6 | 创建辅助 + 一个纯 CPU 函数 |
| `pipeline` | 2 | 管线创建 |
| 合计 | **52** | ②a 的出口：77 → 25 |

本节把其中 **20 个（19 个函数 + 1 个 `deren.vulkan.init_utils` 模块初始化器）** 读透了。

### 从代码里读出来的五条结论（每条都有证据）

**① 契约没有释放虚函数——已按你的裁决补上 `virtual release()` + `object_manager<T>`（见 §12）。**
（这一条我先写错过一次，实测把它改过来了，见 §11.3 的测量：`delete p` 走虚分派时，删除析构 `D0` 与它用的 `operator delete` **都在 DLL 里**，所以"引擎 `delete` 会用错分配器"这个理由不成立。）
补上显式 `release()` 的理由因此改成两条**仍然成立**的：
* `delete` **表达不了"放手但未必销毁"**——而后端在这几个句柄背后维护的是**引用计数 + 内容去重登记册**（结论 ③），所以**同一个资源可能被多个调用方复用**，这个语义必须有入口（名字最终定为 `release()` 而不是 `destroy()`，正是为了把这一点说清）；
* **C ABI 表达不了 `delete`**：非 C++ 宿主只能按名字调 `release()`，正如 `api_core` 只能靠 `deren_destroy_api_core`。
代价：给 7 个既有 tier-1 类型加了纯虚函数 ⇒ 跳 `abi_version`（3 → 4）；`destroy()` → `release()` 的改名不额外跳号（槽位未动）。

**② 引擎的资源所有权今天是靠"声明顺序"表达的，迁移后这个语言保证会消失。**
`runtime::~runtime()` 的注释（`runtime.constructor.cppm:209-212`）写得明明白白："views/samplers/buffers/images are RAII and free themselves as this runtime's members destruct (after this body; **`vulkan_core`, which owns the vma allocator, is declared first and destructs last**, so every `vk_buffer`/`vk_image` still has a live allocator when it releases)"。换成契约句柄后，这个由编译器保证的逆序析构**必须变成显式的顺序义务**（"每个 `image`/`buffer` 必须在其 `api_core` 之前释放"），否则翻转后会出现"释放打到已拆的分配器上"——而且只在关窗/退出时炸。

**③ RAII 包装的删除器是"捕获了分配器的 lambda"，而 `vma_allocator` 其实是一本**按 handle 索引的资源登记册**。**
`vma_handles.cppm:106/201` 的 `reset()` → `vma.cppm:1226/1315` 的 `make_owner`，它交出的是**两个**回调：`retain_image(h)` 与 `free_image(h)` —— 也就是说引擎手里的 `vk_image` 是**引用计数句柄**，不是一个独占所有者。
`free_buffer/free_image(uint64_t handle)` 在 `this->buffers` / `this->images` 这两张表里按**裸 handle 值**查（`vma.cppm:1495-1516`），查不到就静默返回；**引用计数归零才真的 `vmaDestroyBuffer/Image`**（`use_count.fetch_sub(1) > 1`）。图像还带**内容去重**：分配前先算 XXH3-128 摘要，命中就"reuse an existing image without any allocation or upload"（`vma.cppm:1318-1327`），去重键存在 `image_detail::digest`（`vma.cppm:142-147`）。
⇒ 这不只是分配器，而是**资源登记册 + 引用计数 + 内容缓存**。契约的 `image*` 句柄模型三样都没有：`create_image(image_desc)` 既表达不了"相同字节复用同一张图"，也表达不了"这份句柄还有别人在用"。
**"登记册/去重/引用计数留在后端（契约增补 retain/release 语义）还是搬到引擎（引擎自己做缓存）"是 ②a 的第一个架构裁决**，方案里目前完全没有这一条。

**④ 契约的格式词表只有 5 个值，纹理需要任意 `VkFormat`。**
`rhi::image_format` = `unknown` + 4 个 8bit（`rhi.api_core.cppm:103-107`，注释说它只用来说截图怎么解包），而后端的 `image_create_info` 是 `{width, height, mip_levels, array_layers, VkFormat format, VkImageUsageFlags extra_usage}`（`vma.cppm:121-129`）。
⇒ 纹理走契约需要**真正的格式词表 + usage 词表**，否则就得明确写"纹理继续走 escape"。这是 desc 设计里最大的一块。

**⑤ `descriptor_heap` 那 8 个不是所有权问题，是"引擎还在自己录二级命令缓冲"。**
* `push_data(VkCommandBuffer, offset, span)` / `record_bind(VkCommandBuffer)`：**契约的 tier-2 `descriptor_heap` 能力已经声明了 `push_data(command_list&, span<byte const>)`**（`rhi.extension.cppm:163-173`），后端只是没实现它、`abilities()` 也只广播 `vulkan_escape`。所以这一条是"把已声明的能力兑现"，不是新设计。
* `write_image(offset, VkImageViewCreateInfo const&, VkImageLayout, VkDescriptorType)` / `write_buffer(offset, VkDeviceAddress, size, VkDescriptorType)`：收的是**原生 Vulkan 结构/枚举**。要么给契约补 POD 镜像（编号化的格式/布局/描述符类型），要么留 escape。契约注释里已经把 `write_resource_descriptors`/`write_sampler_descriptors`/`bind_resource_heap`/`bind_sampler_heap` 列为"land with S1"，说明设计意图是**补进能力表**。
* `bind_infos(VkBindHeapInfoEXT&, VkBindHeapInfoEXT&)`：填两个原生结构，注释解释了它为什么单独存在——"a SECONDARY COMMAND BUFFER IS VALIDATED ON ITS OWN"（`descriptor_heap.cppm:168`）。但契约的 `begin_commands()` 明确写着"secondary buffers stay the backend's"（`rhi.api_core.cppm:254`）。**所以真正让 `bind_infos` 消失的不是换接口，而是引擎停止自录二级命令缓冲**——这是一条比符号替换深得多的架构项，方案里没写。
* `ready()`/`limits()`/`resource_size()`：查询。`limits()` 返回的 `heap_limits` 里全是 `VkDeviceSize`（`descriptor_heap.cppm:64-78`）⇒ 要跨边界就得有 POD 镜像。

### 溶解类（靠搬代码消失，不靠加契约面）

| 符号 | 为什么不该跨界 |
|---|---|
| `init_utils::default_task_pool_threads()` | 纯 CPU：返回一个线程数。契约里没有任何它的位置，引擎自己算或 `utility` 提供 |
| `init_utils::create_recording_pool(core&)` | 命令池归后端（不变式 1）；契约已有 `frame_begin()`/`begin_commands()` 的池语义 |
| `vma_allocator::log_statistics()` | 查询/日志，属于后端自己的可观测性，不该由引擎在析构里驱动 |
| `vma_allocator::invalidate_if_not_coherent(VmaAllocation, ...)` | host 一致性修补；它跟着"谁拥有 staging buffer"走（今天是引擎的 `vulkan/readback/readback.cpp`） |
| `initializer for module deren.vulkan.init_utils` | 它存在**只因为引擎还在 `import deren.vulkan.init_utils`**；资源面迁完自然消失——它同时也是"引擎不该再导入这个模块"的指示器 |

### ②a 的第一个增量（建议直接落地的顺序）

1. ~~裁决资源句柄的释放语义~~ **已定并已落地**（§12）：`virtual release()` + `object_manager<T>`，语义 = 引用减一、资源可能被复用。
2. **登记册 / 引用计数 / 内容去重放哪边**（结论 ③）：前半**已被 #1 的语义确认留在后端**；只剩**内容键的形状**——`image_desc` 带像素字节还是带引擎算好的 128 位摘要。
3. **定 `image_desc` / `buffer_desc`**：格式词表 + usage 词表 + 尺寸（结论 ④）。
4. **引擎侧 ~58 处声明点改成契约句柄**并写出**显式释放顺序**（结论 ②）：`runtime.declarations.cppm` 17 处、`runtime.constructor.cppm` 6、`primitive` 6+2、`acceleration_structure` 6+1、`ray_tracing` 5+5、`runtime.frames` 4、`runtime.probes` 3、`readback` 1+1、`pipelines` 1、`runtime.cpp` 1。
5. **资源查询改为按值拷贝**（§11.2 结论 2），不要让引擎缓存后端 map 的裸指针。
6. **兑现已声明的 `descriptor_heap` 能力**（`push_data` 先做，它签名早就定好了），并决定 `write_*` 是补 POD 镜像还是走 escape；`bind_infos` 则取决于"引擎还自不自录二级"（§11.2 结论 3）。
7. **出口**：`vma_allocator` + `init_utils` + `pipeline` + `descriptor_heap` 那 14 个操作符号清零、30 个 RAII 符号随成员替换消失 ⇒ 翻转门 **77 → 25**；13 个渲染哈希逐字节、validation 0、A5 影子面逐字段。

> **注意**：第 4 步是这次迁移里唯一"编译器不再替你保证"的地方（结论 ②）。建议在动它之前先加一个**退出期门**：一个只做"建资源 → 立刻销毁 runtime"的 headless 用例，让"释放打到已拆分配器"这类问题在 CI 里现形，而不是在关窗时。

### §11.1 逐符号判定（独立事实核查，20 项）

| 判定 | 数量 | 符号 |
|---|---|---|
| **(c) 只调用一个操作**（对象是后端的，引擎只是用） | **12** | `descriptor_heap` 全部 8 个 + `default_task_pool_threads` + `invalidate_if_not_coherent` + `log_statistics` + `deren.vulkan.init_utils` 模块初始化器 |
| **(d) 引擎按值持有 RAII 包装**（后端分配、后端销毁） | **5** | `create_host_buffer`、`create_host_buffers`、`create_texture_2d`、`vma_allocator::create_buffer`、`vma_allocator::create_image` |
| **(b) 无锁借用**（返回后端 map 里的指针） | **2** | `get_buffer_detail`、`get_image_detail` |
| **混合** | **1** | `create_recording_pool`：池是 **(b) 从 `core` 借的**（`core::make_command_pool` 创建并注册 `vkDestroyCommandPool`），二级命令缓冲是 **(d) 引擎按值持有** |

⇒ **真正"引擎拥有对象"的只有 5 个符号**；那 30 个 RAII 符号是这 5 个**交出去的包装**被引擎到处按值持有造成的后果。所以 ②a 的所有权问题面比"52"小得多，但**爆发面**就是这 30 个。

### §11.2 三条新事实，各有设计后果

1. **堆缓冲其实是分配器在回收，不是堆自己。** `descriptor_heap::~descriptor_heap()` 是空的（`descriptor_heap.cppm:317-321`），`descriptor_heap::destroy()` **一个调用者都没有**；两个堆缓冲只由 `vma_allocator::destroy()` 的遍历释放（`vma.cppm:884-887`），而那次调用来自 `core` 构造时注册的 cleanup lambda（`core.constructor.cppm:182-184`）。
   ⇒ 任何"把堆缓冲交出去"的契约都必须复现这条耦合：**堆的显存不由堆释放**。这也是"契约的 `create_buffer` 若被引擎用来建堆缓冲，谁释放"必须先答的原因。

2. **两个 `get_*_detail` 返回的是"解锁后的借用指针"，而引擎把它缓存成了成员。** 定义在 `vma.cppm:1447-1453 / 1487-1493`，返回指向 allocator 内部 `std::map` 的 `const*`，**guard 在 return 时已经释放**。引擎读 `.allocation_info.pMappedData` / `.buffer` / `.allocation` / `.memoryType`（20+ 个调用点），`primitive` 甚至把指针存成成员（`vertex_detail`/`index_detail`），所以 `primitive.cpp:239-249` 在 `reset()` 之后必须手工置空。
   ⇒ 契约里的资源查询**必须按值拷贝**，不能返回借用指针；否则翻转后这类悬垂指针连"后端帮不上忙"都算不上——它跨了 DLL。

3. **引擎确实在自己录二级命令缓冲，而且规模不小。** `create_recording_pool` 每个阴影级联 × 帧槽一个池、每个录制 worker 一个池（`runtime.constructor.cppm:887,893`），worker 数 = `default_task_pool_threads()`（:873）。这解释了 `bind_infos` 为什么存在（`descriptor_heap.cppm:168` 的注释："a SECONDARY COMMAND BUFFER IS VALIDATED ON ITS OWN"），也确认了结论 ⑤：**这条要么让引擎停止自录二级，要么契约得把次级录制也表达出来**。
   附带确认 `default_task_pool_threads` 是纯 CPU：它只给引擎自己的 `deren::utility::thread_pool`（`runtime.declarations.cppm:1106`）定宽度。

### §11.3 一条悬而未决的问题解决了，而且结论对翻转有利

事实核查代理发现：引擎引用 `core::core(create_info const&)`（构造函数），**却完全不引用 `core::~core`**（D0/D1/D2 都只在后端定义），也不引用 `core` 的 vtable/typeinfo。实测复核（`llvm-nm`）：

| 符号 | 引擎（undefined） | 后端（defined） |
|---|---|---|
| `core::core(create_info const&)` | **有** | 有 |
| `core::~core` D0/D1/D2 | 无 | 有 |
| `core` 的 `_ZTV`/`_ZTI` | 无 | 有 |
| `__shared_ptr_emplace<core>` 控制块 | 引擎自带（weak，5 个） | — |

**解释**：`core` 的析构函数是虚的（继承自 `api_core` 的 `virtual ~api_core() noexcept`），所以 `__shared_ptr_emplace::__on_zero_shared()` 走的是 **`delete p` → vptr 虚分派**；vptr 由**后端侧构造函数**写入，于是真正执行的删除析构 `D0`、以及它用的 `operator delete`，**都在后端**。

⇒ **引擎持有的后端对象，其销毁本来就不需要导出任何符号，而且分配器已经是对的。** 翻转在这一面比 §11 结论①担心的要稳：真正必须消失的是**构造**——`core` 的构造函数那个符号（以及引擎自带的 `make_shared<core>` 控制块）。这正是方案说"引擎持 `shared_ptr<api_core>`、经 `deren_make_api_core` 创建"的原因；而 `deren_destroy_api_core` 作为显式删除器仍然是**更该有的**写法（§4.1 item 3，把所有权转移写明白），只是它不是唯一可行的那条路。

---

## §12 已实现：`virtual release()` + 模板 `object_manager<T>`

**你的两次裁决**：①虚基类改成虚函数，配一个模板（名字待定）在析构时调用它；②**名字用 `release()`**，文档里强调"部分情况下它代表的资源**可能被复用**"。都已落地。

### 语义定稿（第②条落地后的真正内容）

`release()` 的含义**不是"销毁这个对象"**，而是——

> **释放调用方手里的那一个引用；对象是否真的销毁，由后端决定。后端允许把同一个资源服务给多个调用方**（本项目就是这么做的：`vma_allocator` 用 XXH3-128 内容摘要做键，配一个引用计数），所以**最后一个引用释放时才销毁**。

由此推出两条调用方必须接受的后果，都写进了契约文档：
1. **这次调用可能什么都没释放**——需要"显存回来了"的调用方不能从这个调用推断，那取决于后端自己的账（`vma_allocator::log_statistics()`）。
2. **句柄在调用之后即失效，哪怕对象还活着**——调用方手里已经没有引用了，再碰它、或再 `release()` 一次，就是在减别人的计数。

**以及那条必须一并回答的语义缺口：第二次引用从哪来？** 答案是 **"再问工厂要一次"——工厂就是 retain**：`create_image()` 若内容与参数命中一个活着的对象，就返回**那个对象 + 一个新引用**，调用方那一次 `release()` 正好平衡。契约**故意不设 `retain()` 虚函数**：一个"给这个句柄加一个引用"的裸调用，会让调用方为它**从未收到过**的句柄制造引用——正是后端当初把 `free_*`/`retain_*` 私有化、改成发放可注入 owner 时消掉的那条过释放路径（`vma.cppm:213-219` 的注释）。

这条同时说明：**`destroy()` 那个名字是错的**（它暗示销毁），`release()` 才对；而 `object_manager` 的 `reset()` 与今天的 `vk_image::reset()` 现在是 1:1。

### 契约（`promise/rhi/rhi.api_core.cppm`）

* `virtual void release() noexcept = 0;` 加在 **7 个既有 tier-1 句柄**上：`buffer`、`image`、`sampler`、`shader`、`pipeline`、`swapchain`、`query`。
* `command_list` **不加**：它按契约只可能是借用视图（`begin_commands()` 的文档就是这么写的），没有 `release()` 反而让"不能管理它"成为**类型系统**事实而不是纪律。
* 新增"所有权"总纲，写全了上面那三条：**一个引用**、**release ≠ destroy（可能被复用）**、**工厂即 retain（无 `retain()` 虚函数）**；外加"借来的视图"那半——`frame_image()` / `frame_readback_buffer()` / `begin_commands()` 被 `release()` 时给**一次性具名日志**而不是静默 no-op（静默会把"调用方以为自己持有引用"藏起来）。这个"一次性日志"的形状照抄仓库既有的 `frame_commands::use` 对陌生图像的处理。
* `object_manager<object>`：move-only、空置即转移、`reset()` 幂等调 `release()`（**析构后对象可能仍在**）、`release()` 交还引用而不释放、`get()`/`operator->`/`operator*`/`operator bool`/`swap`。并写明**两个 manager 指向同一对象是合法状态**（去重命中就会产生），各持一个引用、各放一个。
  模板名字仍待定，候选写在注释里：`object_manager`（现用）/ `object_ptr`（对齐 `unique_ptr`）/ `owned`——改名只是这一处加调用点的替换。

### ABI

* 给既有类型加纯虚函数 = **跳号**（不变式 3）：`abi_version` **3 → 4**。
* **`destroy()` → `release()` 这次改名不再跳号**：槽位没动（同一顺序、同一个位置），只有符号名变了，所以一个用旧名编译的引擎与用新名的后端**仍然二进制兼容**——契约的规则本来就是"虚表形状变了才跳号"。这一条写进了 `rhi.contract.cppm` 的注释。

### 三个实现者（全部实现，编过）

| 实现者 | `release()` 的行为 | 为什么 |
|---|---|---|
| `core::frame_image_slot`（真后端，**借用**） | 一次性具名日志，**不减任何引用** | 它是 `core` 的成员，交换链图像归 core |
| `core::frame_readback_slot`（真后端，**借用**） | 一次性具名日志，**不减任何引用** | readback 槽是 core 自己的分配 |
| `probe_buffer`（探针，**静态存储**） | 置 `released` 并清零 `size_bytes` | 静态存储没有引用计数可减，但**这次调用必须到达**——可观测才能被测 |

### 测试与门（都跑过）

* `test_dynamic_link.cpp` 的"**拥有引用经契约释放**"一段：`object_manager<buffer>` 接管 → `size()==128` → move 后源为空 → `release()`（交还引用）后仍 `size()==128` → 再调 `release()` → `size()==0`，并验证空 manager 的 `reset()` 幂等。**DLL 半与静态半走同一套断言**，所以"释放确实跨过边界、落在库里"是可证的，不是被假设的。
* 产品树：全目标构建 **exit 0**、`ctest` **14/14**（`test_dynamic_link` 109 checks / 0 failed）、`clang-format-check` **exit 0**。
* 尖刺树（SHARED）：`deren_abi_version()` = **4** == 本 exe 编译的 4，11 checks / 0 failed；DLL 导出**仍正好 3 个**——加这些虚函数没有破坏窄导出。
* **翻转门不变：77 / 226**（引擎还没开始调用 `release()`，符号集自然不动——"契约先行、调用点随后"的预期）。

### 对 ②a 的第一步意味着什么

`release()` 一落地，§11 结论①里"资源模型未定"的那一半就有形状了：引擎侧那 ~58 处声明点将来持 `object_manager<rhi::image>` / `<rhi::buffer>`，作用域退出即释放，**声明顺序那条纪律仍然由 C++ 保证**（结论②的担忧随之减半）。
**登记册 / 引用计数 / 内容去重经这次裁决确认留在后端**（"资源可能被复用"就是它的语义表现）；§6 #6 剩下的只有**内容键的形状**：`image_desc` 是带像素字节（今天 `create_texture_2d` 的做法）还是带引擎算好的 128 位摘要（`deren::utility::xxh3_128bits` 引擎侧可用）。

---

## §13 设备路径实测通过；以及一个**新的翻转风险：卸载 DLL 会死锁**

`tests/spike_backend_boundary.cpp --with-device` 现在**跑通并正常退出**：**20 checks / 0 failed / exit 0**（安全模式 11 checks）。

| 项 | 结论 |
|---|---|
| **Q2 真 DLL 的删除器（强形式）** | ✅ **实测通过**。`deren_make_api_core` 在 DLL 里建出**完整 context**（instance/device/queue/swapchain/descriptor heap，后端 `debug.log` 逐行可查），`shared_ptr` 用**从 DLL 解析的** `deren_destroy_api_core` 释放，teardown 正常返回 |
| **Q4 BMI 跨 target（强形式）** | ✅ **实测通过**。`abilities()` / `query_extension()` 是对 DLL 内构造对象的虚调用，经本 exe 编译的虚表分派，结果正确 |
| S3 门 | ✅ `create_buffer` 跨边界答 `nullptr` 而不是崩 |

### 新风险（必须进步 ④ 的门）：`FreeLibrary` 永不返回

**实测**：全部步骤跑完后（路标显示 `core teardown returned`、`all checks done`），进程停在 `cpu=0.45s / run=80s+ / threads=10 / mem=76MB 不动`——唯一剩下的动作是 loader 析构里的 `FreeLibrary`。**控制实验**：把"卸载"这一步去掉（故意泄漏 library 句柄），同一个测试立刻 `exit 0`。所以挂的是卸载，不是测试内容。

原因方向：DLL 的 **process-detach 路径**在等一个它永远不会得到的东西——GLFW 是在 DLL 里初始化的，而**这个后端按设计从不调用 `glfwTerminate`**（`core.constructor.cppm` 明确写着"does not initialise GLFW on this path and never terminates it on any path"），于是 detach 里等待那部分状态就是死锁，而不是"慢"。

**为什么这是产品问题而不是测试细节**：不变式 4 说"DLL 活到进程结束"，所以产品**不需要**卸载——但 `dynamic_link::library` 的**析构函数无条件卸载**，而引擎的 loader 对象会在退出时被析构。**一次会在关窗/退出时死锁的翻转，正是那种直到有人真去关窗口才暴露的失败。**

**处理（已完成，2026-10-03）**：
1. ✅ **`dynamic_link::library` 增加 `detach()`**：交还平台句柄并把自己置空，析构于是**不会卸载**；detach 之后 `native_handle()` 为 null、`symbol()` 具名拒绝（"the library is not loaded"），所以它是调用方对 loader 的**最后一次调用**。**有测试**：`tests/test_dynamic_link.cpp` 的 `test_a_detached_library_is_not_unloaded` 用既有的"映射中的映像不能被删除"手法证明 **detach 之后文件仍被映射**（即确实没卸载），而隔壁那个 `test_the_library_is_unloaded_when_it_goes_out_of_scope` 继续证明"默认仍会卸载"。115 checks / 0 failed。
2. ✅ 尖刺改用 `detach()`：安全模式 11 checks、设备模式 **20 checks**，**两种模式都自己退出（exit 0）**。
3. **步 ④ 的要求**：引擎的 loader 成员必须走 `detach()`（或等效地永不卸载）——这就是 §11 结论②里"`runtime` 要在 `core_owner` 之前声明一个 `dynamic_link::library`"的那个成员。`close()` 仍然是给**小型、无 GLFW 状态**的库用的（probe 的测试就在用），不是给后端用的。
4. **门**：**已经有了，不需要新工具**——`scripts/windows/check_render.ps1` 每个场景 `WaitForExit(180000)`，超时判 `timed out`、退出码非 0 判失败（:376-377）。翻转之后这条从"顺带"变成**承重**：产品若在退出时死锁，14 个场景会全部 `timed out`。所以步 ④ 的门清单里，这条要写成"**13 哈希逐字节相同** *并且* **14/14 在 180 s 内正常退出**"。

### 顺带记一条测试方法学（本次踩坑）

第一次跑 `--with-device` 时我把 stdout 重定向到文件，结果进程被杀后**文件是空的**：`fwrite` 到重定向流是**块缓冲**，没 flush 就什么都没落盘——于是我拿"空输出"当成了"没运行"。现在测试用**无缓冲、每条都 `fflush` 的 stderr 路标**（`mark()`），挂在哪一步是可见的。跨进程边界的测试，**输出必须先保证落盘**。

---

## §14 门工具本身的两个问题（跑过才发现的）

1. **本机参考集是 10-01 的旧集，与方案 §5 自己那份"冻结 13 个"不一致。**
   本次实测的实际哈希**逐个对上方案 §5 的冻结值（11/13）**：`deferred 972A31EC5FF55C87`、`deferred_taa_fxaa 4021B16AFDB2F43E`、`deferred_ssao_off BFE3A472FBAB0B5E`、`shadow_single A92C5965316679F3`、`unlit F3C2D7FEFDAD864F`、`transparent_blend CC7F77F93487AA5E`、`sponza 50AF7E46CC1E2A92`、`metal_rough_glossy A1AFBFB61DBFD104`、`glossy_motion 9F31E89BE38B771C`、`deformation 723569BA0D03640C`、`laevatain_old_chain 190EB09D3E9FDCDA`。
   而脚本默认用的 `%LOCALAPPDATA%\deren\baseline` 里那套是 **2026-10-01 21:57** 的，于是同样这 12 个场景全部报 `CHANGED`。
   ⇒ **"跑一次 `check_render.ps1 -Full` 就是绿"这句话目前不成立**；能成立的判据是"产物是否复现 §5 那 13 个数字"，而那是 **11/13**。
   ⇒ 两个 toon 场景与 §5 不同（`laevatain_goo_toon` 实测 `CF5A34D8DF6B6FFC` vs §5 `00451C49384D0337`；`_body` 实测 `C3365CEEB8AD3723` vs §5 `4D9C9B472F37DC6B`），但**同一资产的旧链 `laevatain_old_chain` 完全对上**，所以差异被定位在改写过的 toon 链本身。**这两个数字该由你确认**（重写后的链是否在 §5 写下那串数字之后又变过像素）。`laevatain_no_sidecar` 崩 `0xC0000409`（既有的缺 `chars/laevatain.glb`）。
   **没有动 baseline**：脚本自己写着要先归档旧集、写明理由再 `-Update`。
2. **脚本注释与代码的环境变量名不一致**：注释说 `VR_RENDER_BASELINE_DIR`（:63），代码读的是 **`DEREN_BASELINE_DIR`**（:97）。照注释设置会静默落到默认目录——正是"指错了参考集却以为在比对"的入口。

**判据本身没问题**（已核实）：门的哈希 = **PNG 文件字节的 sha256 前 16 位**（`deferred` 的 sha256 前 16 位正是 `972a31ec5ff55c87`），我逐张看过 actual，是真实渲染帧（1080×960）。

---

## §15 ②a 第一块已落地：**第一个被拥有的资源跨过了契约（`buffer`）**

### 契约侧：`buffer_desc` 的形状是量出来的，不是想出来的

先量了引擎的 **12 个创建点**（`vulkan/runtime`、`readback`、`ray_tracing`、`acceleration_structure`），它们要求的东西正好三类：

| 字段 | 内容 | 依据 |
|---|---|---|
| `buffer_usage`（10 个值） | **内存意图**，不是 Vulkan 内存属性：`vertex`/`index`/`uniform_gpu_only`/`uniform_coherent`/`uniform_cached`/`storage_coherent`/`readback_coherent`/`acceleration_structure_storage`/`acceleration_structure_scratch`/`storage_gpu_only` | 12 个点用到的 `buffer_type` 全集，1:1 |
| `buffer_flags`（3 个位） | **额外能力**，按"让调用方能做什么"命名：`device_address`、`acceleration_structure_input`、`micromap_storage` | 12 个点用到的额外 `VkBufferUsageFlags` 只有这 3 种 |
| `initial_bytes`（`std::span<std::byte const>`） | 创建时的初始内容；**空 = 只分配** | 与"内容去重"直接相关（见下） |
| `struct_size` | 与 `create_info` 同一条 append-only ABI 守卫 | 一致 |

**为什么初始字节属于 desc**：①"创建 + 上传"是一个动作而不是两个——调用方少一次 API 往返、少一个中间状态；②**对图像**，后端按内容（XXH3-128）建键，只有在创建时拿到内容才可能识别"同一份内容"。
**一处更正（写手核对注释时量出来的）**：`buffer_detail` **今天没有内容摘要**（图像才有），所以对 buffer 而言"内容去重"这半条**并不成立**，成立的是①。契约里 `buffer_desc` 的注释仍带着那个过度声称，等验证跑完再改——那是纯注释，但改 `.cppm` 会触发重编译，从而作废验证者正在认证的版本。

### 后端侧：工厂真的建东西了

* `core::create_buffer` 从"答 `nullptr` 的 S3 占位"变成真实现：**ABI 守卫** → `buffer_usage` 逐值映射到 `buffer_type`（**故意写成 switch 而不是 cast**：新的契约值不许悄悄变成这个整数在这里的含义）→ `buffer_flags` 映射到那 3 个 Vulkan 位 → `vma.create_buffer(初始字节, size, type, extra)`。
* 成功则交出一个**堆上**的 `owned_buffer : rhi::buffer`，它持有 `vk_buffer`（RAII）并缓存 size 与映射地址。`release()` = **`delete this`**：析构里 `vk_buffer` 的析构 → release lambda → `free_buffer`，也就是**分配器的引用计数减一**——与 §12 定的"release 未必是销毁"完全对上。
* **两条既有纪律被遵守**：
  1. ABI 守卫的实现只有一份了：`covered_by` 放进 `core.declarations.cppm`，上下文描述符（`core.constructor.cppm`）和 buffer 描述符（`core.api_core.cppm`）都走它——顺手把原来的本地 `covers` 去掉了；
  2. `get_buffer_detail` 是**解锁后的借用**（§11.2 结论 2），所以只把**值**（映射地址）拷出来，**绝不保留**指向分配器 map 的指针——这正是那条结论要求的写法。

### 实测（真设备）

| 项 | 结果 |
|---|---|
| 尖刺 `--with-device` | **27 checks / 0 failed / exit 0**：宿主可见 64 B（带初始字节，`mapped()` 非空且尺寸对）、GPU-only 256 B（`mapped()` 空）、0 字节具名拒绝、两次 `release()` 后 teardown 正常 |
| **像素中性** | 门重跑，`deferred` 仍是 **`972A31EC5FF55C87`**，与改动前、与 §5 冻结值逐字节相同（引擎还没有任何路径走新工厂） |
| 产品树 | 全目标构建 exit 0、`ctest` **14/14**、`clang-format-check` exit 0 |
| 翻转门 | 仍然 **77 / 226**——**这是预期的**：契约与后端有了，但引擎的 12 个创建点还没换过来，"调用点随后" |

### ②a 还剩下的

1. `image_desc`：格式/usage 词表（契约的 `image_format` 今天只有 5 个值，纹理要任意 `VkFormat`）+ §6 #6 的**内容键形状**（本次 `buffer_desc` 已经给出方向：**内容随 desc 走**）；
2. 引擎那 **~58 处声明点**（`runtime.declarations.cppm` 17 处…）换成 `object_manager` + 契约句柄，并写出**显式释放顺序**（动手前先加"建资源 → 立刻销毁 runtime"的退出期门）；
3. `descriptor_heap` 那 8 个操作符号（其中 `push_data` 的契约签名早就写好了）。

---

## §16 第二个能力落地：`device_address`（以及一次被实测抓住的自造 bug）

### 一个被"能力自己的规则"逼出来的契约改动

`device_address` 本来**装不出来**，因为它同时声明了 buffer 地址和 **AS 地址**，而后者需要一个 `acceleration_structure`——契约还生产不出来。按能力自己的规则（"置位 ⇒ 取得到 且 **用得上**"），一个不可达的操作会把**整个 bit** 拖住。

所以把 `acceleration_structure_address` **移到了 `ray_tracing`**（唯一能交出那个操作数的能力）。这不是收窄，是**一个操作数一个能力**：合并在一起的代价是"永不广播或广播错"。
* `abi_version` **4 → 5**（两个 tier-2 能力同时改了形状：一个少一个虚函数、一个多一个）；
* probe 与 `tests/test_dynamic_link.cpp` 同步；`abilities()` 现在报 `vulkan_escape | device_address`。

### 后端：地址是真的

`buffer_address_view` 实现 `buffer_address()`：`VkBufferDeviceAddressInfo` + `vkGetBufferDeviceAddress`，**offset 带上**；对没声明 `buffer_flag::device_address` 的 buffer 答 **0**（Vulkan 只给声明了 usage 的 buffer 发地址——"没要就没有"，不是失败）。**没有 RTTI 所以没有类型核对**：工厂对象不是单例，无法像 `use()` 那样比指针，所以这条前提是"调用方只能问这个后端交出去的句柄"——契约自己的规则，**写在注释里而不是假装检查过**。

### 被实测抓住的自造 bug（值得记一笔）

第一版 `buffer_address()` 一律答 0。尖刺报了 2 个 FAIL，然后**后端自己的日志把三次可能性缩到一次**：

```
rhi: create_buffer 64 B usage 5 flags 0x1 -> extra 0x20000, native 0x210…2d0, mapped true
```

buffer 建得完全正确（`flags 0x1`、`extra 0x20000` = `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`、native 非空），而"答 0"的日志**一行都没有**——说明 `buffer_address` 走了**更早的提前返回**：`owner == nullptr`。原因是我加了 `address_view` 成员却**忘了在 `core` 构造函数里给它设 owner**（另外四个 view 都在那里设）。补上后立刻 **33 checks / 0 failed**。

⇒ 两个副产品：**（a）`bufferDeviceAddress` 特性本来就是启用的**（地址非零），所以那个 0 从头到尾都是我的 bug，不是设备问题——如果我一开始就靠"读代码猜特性"，会往完全错误的方向查；**（b）"答 0 时打一条日志"这种诊断让三次可能性一次性收敛**，比再猜一轮便宜得多。

### 验证

| 项 | 结果 |
|---|---|
| 尖刺 `--with-device` | **33 checks / 0 failed / exit 0**（`abilities()` 含 `device_address`；带地址的 buffer 答非零且 offset 正确；未声明的答 0） |
| **像素中性** | 门重跑，`deferred` 仍 **`972A31EC5FF55C87`** |
| 产品树 | 全目标构建 exit 0、`ctest` **14/14**、`clang-format-check` exit 0 |
| 翻转门 | **77 / 226**（引擎还没换过来；这次只动了契约与后端的**能力面**） |

### 这一步之后，引擎侧 buffer 迁移的看门人已经打开

引擎的 12 个创建点里，凡是写描述符堆/AS 的都依赖设备地址；现在 `create_buffer` 与 `device_address` 两件都有了，**引擎的 buffer 面可以开始换了**——下一块就是它（~58 处声明点里的 buffer 部分），换完才轮到 `image_desc`。

---

## §17 ③ 图像面设计（已定，待契约冻结期结束即落地）

契约现在**必须冻结**：两个写手正对着它编译（`kit-runtime` 换 `vulkan/runtime`、`kit-resources` 换资源侧），我此刻改它会让他们的基线漂移。所以图像面先做设计。

### 普查结果（引擎侧的图像创建点与用途）

| 用途 | 尺寸/层 | 格式 | 额外 usage | 今天怎么创建 |
|---|---|---|---|---|
| 阴影贴图 | `shadow_map_size`² × 层数=活跃级联 | **`vulkan_core.depth_attachment_format`** | `SAMPLED` | `vma.create_image(texture_2d_depth)` |
| 环境立方图 | 立方图 + mip | `R16G16B16A16_SFLOAT` | — | `vma.create_image(texture_cubemap)`，内容来自 `initial_bytes` |
| 辐照度立方图 | 同上 | `R16G16B16A16_SFLOAT` | — | 同上 |
| BRDF LUT | 2D | `R16G16_SFLOAT` | — | 同上 |
| post LUT | 2D | `R8G8B8A8_SRGB` | — | 同上 |
| goo FGD LUT | 2D | `R8G8B8A8_UNORM` | — | 同上 |
| 白纹理/贴图槽 | 2D | `R8G8B8A8_UNORM` / `_SRGB` | — | 检查点之外的纹理装载 |
| GPU 探针目标 | 2D | 由探针选 | — | `vma.create_image(texture_2d)` |

### 三个设计决定

**(1) 格式：能按角色说的就按角色说，只有确实要紧的才逐个命名。**
阴影图的格式今天来自 **`vulkan_core.depth_attachment_format`**——引擎在**读后端成员**决定格式（又一处 §11 级耦合，且它连契约方法都不是）。契约的答案是 `image_format::depth`：**引擎说"我要一张深度图"，后端挑具体格式**（挑哪个是设备能力问题，不是调用者的问题）。反过来，颜色/LUT 这类**字节布局要紧**的（内容由 CPU 上传、由采样器按格式解读）必须逐个命名：`r8g8b8a8_unorm` / `r8g8b8a8_srgb` / `r16g16_sfloat` / `r16g16b16a16_sfloat` / `r32g32b32_sfloat`（今天的 5 个值 + 这 5 个）。
⇒ 判据：**调用者是否需要知道字节布局**。需要→命名；不需要（只要"一张深度图"）→角色。

**(2) `image_desc` 形状**（与 `buffer_desc` 同构，同一条 append-only 守卫）：
`struct_size` / `extent`（复用 `image_extent`）/ `mip_levels` / `array_layers` / `format` / `flags` / `initial_bytes`。
`image_flags` 按普查命名：`sampled`（今天唯一的额外位）、`storage`、`color_attachment`、`transfer_source`、`transfer_destination`、`cube_compatible`。
**内容仍在 desc 里**（§15 定的方向）：IBL/LUT 五个创建点全是"内容 + 一次上传"，后端的内容去重只有在创建时看到内容才成立。

**(3) 视图：引擎要的是"数组视图"和"整图视图"，不是裸 `VkImageView`。**
引擎持有 `shadow_layer_views`（每级联一层）与各纹理整图视图。所以契约需要一个**不是** `VkImageView` 的视图概念：`image::make_view(image_view_desc{ base_layer, layer_count, base_mip, mip_count, role })`——视图只描述**取哪一段**，`VkImageView` 由后端造并通过 `vulkan_escape::native_image_view()` 借出。这样 `make_depth_layer_view` 这类调用在契约里就是"取第 i 层"。

**落地顺序**（写手 buffer 阶段结束后）：契约加 `image_format` 值 + `image_flags` + `image_desc` + 视图接口（**新增接口不跳 abi**，但给 `image` 加虚函数要跳，届时记原因）→ 后端 `create_image`/视图真实现 + `native_image`/`native_image_view` → 尖刺断言（真图像 + 视图 + release）→ 两个写手进第二阶段换图像。

### 顺带记录：写手们交上来的两个契约缺口（已裁决并落地）

`buffer_flag` 补了 4 个位，每一个都来自**普查而不是发明**：`storage`（引擎把逐槽 uniform 块写成 STORAGE 描述符、Slang 按 StorageBuffer 指针读——**位缺失是"读成 0 且没有任何校验报警"的静默错误**，不是 validation 能抓的）、`indirect`、`shader_binding_table`（pass 上传路径唯一的未知位）、`micromap_build_input`。另外把 `render_environment` 的 `buffer_address` 钩子从 `(void*, VkBuffer)` 改成 `(void*, rhi::buffer const&)`——**钩子若不改，每个 primitive 都得留着裸句柄，也就是留着这次迁移要拆掉的东西**。

---

## §18 度量的一条机制事实（改变了 77 这个数字的含义）

验证者在冻结前做的校准里出现了一个必须写下来的机制事实：

**通过具体后端类型调用 → 引擎半侧产生一个未定义符号（后端定义它）→ 该符号"加入"77 的工作清单。通过契约接口类型（`rhi::api_core&`、`rhi::device_address*`、`rhi::vulkan_escape*`）调用 → 虚调用只加载 vptr 并索引 → 一个符号都不产生。**

连 `_ZTV` 也不会被拖进去，因为**引擎从不构造也不析构后端的具体类**（`deren_make_api_core` / `deren_destroy_api_core` 在 DLL 里做这件事）。

所以：
1. **77 这个数的真实含义是"引擎对后端具体类型的直接耦合"**——正好是这次翻转该拆掉的东西。它下降才是进步，而不是"某种不可避免的调用成本"。
2. **迁移若写错会把这 77 变大**：`static_cast<rhi::api_core&>(core).create_buffer(...)` 是进步，`core.create_buffer(...)` 是退步。这条规则已下发给两个写手。
3. **验证者预登记的三个"加入者"**（`core::core::create_buffer`、`core::frame_escape::native_buffer`、`core::buffer_address_view::buffer_address`，外加两个 `_ZTV`）因此不是"预期成本"，而是**缺陷探测器**：它们只要出现，就说明某处写成了具体类型调用。
4. 最终报数必须是**按集合算的** leavers / joiners / net 三个数，net 不等于迁移的规模。同理，验证者还会抽查"离开者"是否**真的换成了契约调用**——棘轮只拦增长，一个符号也可以因为逻辑被内联/复制而离开，那是坏理由。








---

## §19 buffer 面迁移完成（团队分工 + 实测数字）

### 团队怎么切的，以及为什么这样切

**唯一的写冲突点是契约文件**，所以：
- **契约 + 后端核心（`promise/rhi/**`、`vulkan/core/**`）= 我（Lead）**。两个写手同时改一个契约文件＝基线漂移；我在两个写手开工期间**主动冻结契约**，把图像设计（§17）先写成文档而不是先写代码。
- 引擎侧按**文件域**切成两个写手：`vulkan/runtime/**`（16 个创建点 + 17 处明细读取）与资源侧四模块（ray_tracing / acceleration_structure / primitive / readback；pipelines 与 core/filter 确认无 buffer 点）。
- 第三个成员是**独立验证者**：不写任何产品文件，**先预登记验证程序**（集合算术、逐符号归因、抽查 leaver 是否真的换成了契约调用、所有权审计、自己跑门），再动手。

### 写手交上来的四个契约缺口——按"普查而非发明"补进去

| 新 bit | 依据（写手普查） | 为什么缺了它更糟 |
|---|---|---|
| `storage` | 4 个逐槽 uniform 块写成 STORAGE 描述符、Slang 按 StorageBuffer 指针读 | **读成 0 且没有任何校验报警**的静默错误，validation 抓不到 |
| `indirect` | mesh 间接命令表 | 描述符/绘制路径需要一个能表达它的位 |
| `shader_binding_table` | pass 上传路径唯一的未知位 | 其余位按名 panic，而不是悄悄丢掉 |
| `micromap_build_input` | ray_tracing 的 3 个 setup buffer | 与 AS build input 是**不同的 Vulkan 位** |

另外把 `render_environment` 的 `buffer_address` 钩子从 `(void*, VkBuffer)` 改成 `(void*, rhi::buffer const&)`：钩子不改，每个 primitive 都得留着裸句柄——也就是留着这次迁移要拆掉的东西。

### 度量（两个写手各自独立量过，数字一致）

| 指标 | 前 | 后 |
|---|---|---|
| 跨边界符号 | 77 | **66**（leavers 11，**joiners 0**） |
| owning-STL | 1 | **0** |
| 引用点 | 226 | **180** |

**joiners 0 不是一个巧合，是 §18 那条机制的直接结果**：工厂与能力调用一律经 `rhi::api_core&` / `rhi::device_address*` / `rhi::vulkan_escape*` 的**虚调用**，所以不产生新符号。两个写手各自用 mangled 集合差分 + 对象级 `llvm-nm` 独立验证（并顺手证明 residual 里的 `destroy(vma_allocator&)` 是**引擎半侧自己定义**的重载签名，不是边界符号）。

### 两条具名的处置，而不是悄悄留下

1. **`runtime.cpp` 最后一处裸 `vkGetBufferDeviceAddress`**：原因是 `instance_table()` 把契约 buffer **收窄成裸句柄**再交出来，运行时拿不到 `rhi::buffer` 去问地址。裁定加 `instance_table_buffer(uint32_t) -> rhi::buffer const*`（**加法而非替换**，`instance_table()` 保持原样给其他调用者），同一版本内把残留收掉——`grep` 现在只剩注释里的一个词。
2. **`laevatain_no_sidecar` 跑不了**：场景配置指向 `chars\laevatain.glb`，该文件在这台机器上**不存在**（仓库内所有 `chars/`、8 个 build 目录、yzk 下 104 个 build 类目录、D:\ 与桌面整树都搜过）。它的 `.last.png` 停在 10-02 14:25，其余 13 个场景当晚都重新出图 ⇒ 这是**门的覆盖缺口（输入资产缺失）**，不是迁移回归。待你给路径或裁掉这个场景。

### 行为门（两个冻结值逐字节相同）

`deferred` = **`972A31EC5FF55C87`**、`laevatain_old_chain` = **`190EB09D3E9FDCDA`**；写手与验证者各自独立跑出。另有 `ctest` **14/14**、`clang-format-check` exit 0、全目标构建 exit 0。

### ②a 剩下的

1. **图像面**（§17 已定设计）：`image_desc` + `create_image` + 视图 + `native_image`/`native_image_view`，然后两个写手进第二阶段；
2. `descriptor_heap` 那 8 个操作符号；
3. 引擎剩下的 `core::core` 成员面（`make_image_view`、`write_heap_*` 等仍走具体类型的地方）。
