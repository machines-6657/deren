# deren 正式动态后端迁移交接与交叉验证报告

> 归档说明：正文是 2026-10-04 15:48 停工现场，文内“未提交/未 push”均指该时点。之后现场已保存为 `aaa99cbe3170628aa3a27dfda21158ab40ffdeb6` 并上传 fork。用户随后要求上传报告并同步主仓库；本次仅补 Git 归档与同步，不继续开发或运行测试。


记录时间：2026-10-04 15:48，Asia/Shanghai。源码：`D:\deren-workspace\deren`；构建/证据：`D:\deren-workspace\work`。

**结论：本轮迁移未完成，尚未通过完整编译，所有新增迁移代码均未提交、未 push。不能将工作区或旧 build-release 产物当成完成版。** 用户最新要求是“本轮完成正式动态后端，GPU 验收放到异机”，但五小时额度到剩余 5% 时必须停工写交接。资源代理读到 used95% 后停写，父代理随后读到 used96%（剩4%）；目前只整理报告和保存证据。用户最后明确要求“写交接文档”。没有使用重置券，没有安装或更新驱动。

## 1. 用户决定、范围和禁止事项

- 完整实现正式动态后端，不仅是共享 GLFW。允许修复、本地 Git 提交和云端正常 push。
- 应用、Vulkan 后端、ImGui 共用同一份 GLFW 动态库。应用管理初始化、窗口、事件/回调、销毁及终止；后端借用带 `window_system::glfw` 标签的 `GLFWwindow*`，调用 GLFW 创建 Vulkan surface。不要把 GLFWwindow 强转成 HWND，也不要恢复已撤销的主机原生句柄桥。
- 本机不运行 CTest、窗口探针、产品窗口或 GPU 验收。允许必要 configure、compile、静态边界与 PE 检查。GPU 验收由用户在异机完成。
- 用户拒绝 NVIDIA Vulkan Developer Beta 安装；这项拒绝仍有效，不重复申请或安装驱动。
- 五小时额度剩余 <=5%：暂停修改、构建、提交、push、派新任务，保存现场并写交接。不要把下一次额度恢复自动当成用户授权恢复。
- 2026-10-03 23:55 是旧单次截止，已过，不推定为每天重复。
- 保持 D 盘工作区，不重建 C 盘副本、不改 AGENTS/Skills/机器配置、不覆盖脏源码、不强推、不合并上游。

## 2. Git 与现场快照

| 项目 | 当前核对结果 |
|---|---|
| 分支 | `codex/dynamic-link-v3` |
| 本地 HEAD | `ec39712cfb4c05d398a58a9c48a62b4a067d0bc1` |
| origin | `https://github.com/machines-6657/deren.git` |
| 实时远端分支 HEAD | 同上；本轮 `git ls-remote origin refs/heads/codex/dynamic-link-v3` exit0 |
| 上游参考 | `https://github.com/YzK0741/deren`，此前无上游写权限；正常推送 fork 分支 |
| 本轮提交/push | 均没有 |
| 已跟踪差异 | 64 files changed，1212 insertions，1248 deletions；不包含 untracked 新文件 |
| 源码现场备份 | 80 个修改/新增文件；包含 untracked |

保存的证据：

- `work\handoff-git-status-2026-10-04.txt`：最终 Git 状态。
- `work\handoff-diff-stat-2026-10-04.txt`：已跟踪差异统计。
- `work\formal-migration-wip-2026-10-04.patch`：Git binary diff，仅已跟踪文件；**不能单独依靠它恢复新增文件**。
- `work\handoff-source-2026-10-04\`：80 个现场文件，保留仓库相对目录。
- `work\handoff-source-2026-10-04\source-manifest.json`：相对路径、大小和每文件 SHA256；清单自身 SHA256 为 `e9d1e4aa7cff1e4536e496ef48938847e0deecdd9ff47f7fe6a0403e15ff9543`。
- `work\handoff-remote-head-2026-10-04.txt`：实时远端 SHA。
- `work\handoff-diff-check-2026-10-04.log`：`git diff --check` exit0，仅 LF/CRLF 提示；不证明编译或行为正确。

快照属于未验证 WIP，不是新的完成提交。优先在原工作区继续；不要通过 reset/clean 丢掉现场。

## 3. 对照上一份报告：之前做过什么，本轮增加什么

### 3.1 之前已提交的工作

原报告：`D:\deren-workspace\HANDOFF-TO-CODEX-2026-10-03.md`；迁移记录：仓库 `DYNAMIC_LINK_V3.md`、`DYNAMIC_LINK_V3_RESULT.md`。

原报告记载 V2/V3 的契约、窗口所有权、动态探针、全消费者边界检查，以及将具体后端符号基线收紧至 66。它明确没有完成产品正式 SHARED 翻转。原 report 中的构建/CTest/边界结果是当时静态产品的结果，不能移用到本轮迁移。

之后已提交并已在 fork 云端的 `ec39712` 修复四项历史缺陷，详见仓库 `TECHNICAL_BUGFIXES_2026-10-04.md`：

| 编号 | 已提交修复 |
|---|---|
| F1 | untyped 功能查询漏链；不再被可选 micromap/光追节点截断 |
| F2 | 可选属性链截断 RT/AS 属性；逐项按条件追加有效尾部 |
| F3 | 选卡过早接受缺强制能力的第一块 GPU；逐候选检查并打印缺项 |
| F4 | BC/ETC/EAC/ASTC 上传尺寸/mip/layer offset 错误；按压缩块计算并检查溢出 |

旧证据目录 `work\structural-audit\`：capabilities-red.log、texture-layout-red.log、build-fixes.log、ctest-fixes.log、boundary-fixes.log、format-fixes.log。旧记录：CTest17/17、能力用例37/37、纹理布局78/78；旧边界仍是66 symbols /180 references /3 consumers。**本轮没有重跑这些用例，也没有重新证明边界数量。**

`work\CODE-SCORE-PERFORMANCE-2026-10-04.md` 是评估，不能当成性能优化已实施。本轮没有测量帧耗时、上传成本或性能收益。

### 3.2 用户反馈后曾完成的窗口设计和临时探针

`work\FORMAL-DYNAMIC-BACKEND-2026-10-04.md` 已将窗口方案改成共享 GLFW。此前临时 `work\shared-glfw-probe\` 双模块检查11/11通过：共享模块、函数、窗口、错误与终止状态。它发生在用户后来“本机先不测试”的指令之前；没有 Vulkan instance/device/surface，不是产品或 GPU 验收。

上述方案文件尾部仍保留“必须先本机静态 GPU 基线、产品未实施”等旧阶段描述。最新用户授权已覆盖执行顺序，本报告是当前状态索引；下一轮应协调更新旧方案和 V3 结果文件，不能继续引用旧段落声称本轮没有源码修改。

### 3.3 本轮实际落盘，但尚未验证的实现

| 范围 | 文件 | 目前实现意图/状态 |
|---|---|---|
| 共享 GLFW | `cmake/SharedGlfw.cmake`、root CMake | 单一 imported SHARED；Windows GLFW_DLL；选择配套导入库；复制 EXE 同目录 GLFW。共享基础 configure 成功，产品本轮未运行 |
| 异机窗口检查 | `tests/shared_glfw/{probe.h,backend.c,host.c}` | 11项检查入口，option默认OFF、不注册默认CTest；本轮未执行 |
| ABI7资源契约 | `promise/rhi/rhi.api_core.cppm`、contract/core_desc/extension | image/view/sampler/shader/graphics/compute/RT pipeline/query/swapchain 描述和工厂；image sample_count、扩展格式；窗口标签；object_manager持context token |
| 后端资源 | `vulkan/core/core.resources.cpp`、api_core/declarations | 实际工厂与native getters、view保留image、shader对齐、heap-native pipeline、真实query/swapchain facade；未完整编译 |
| 后端原生服务 | `vulkan/core/core.native_services.cpp`、rhi.extension | 既有query_extension(vulkan_escape)的POD协议：snapshot/frame/command/timing/heap/同步提交，无额外导出入口 |
| 加载器与寿命 | `utility/backend_context.cppm`、backend_platform.*、dynamic_link | EXE绝对路径、严格manifest、SHA256、ABI/compat/shader/GLFW校验、成功加载后进程pin、DLL删除器 |
| 包生成 | `cmake/FormalBackendPackage.cmake`、GenerateBackendIdentity/StageBackendPackage、`scripts/windows/package_dynamic_backend.ps1` | 构建指纹、版本目录、真实依赖收集、三入口/EXE零后端导入检查、检查成功后写backend.selection；没有成功完成打包/PE检查 |
| 引擎所有权 | `vulkan/engine/engine_gpu.cppm`、engine_device.cppm | 宿主gpu_context和资源包装；render target及pipeline recipes迁到引擎；资源创建/释放经契约，设备/队列经服务；该适配层仍须审查，不能当作冻结架构 |
| 产品调用迁移 | runtime/filter/pass/primitive/AS/ray_tracing/readback/init_utils/main | 移除大部分具体core import/type；pass_context新增gpu；main校验GLFW后加载后端；CMake改正式SHARED并去宿主后端链接；有待编译发现剩余调用点 |
| 队列同步 | readback、runtime.probes、native services、VMA | 读回/探针经submit_and_wait；native帧提交/present/等待与上传共用queue_sync；录制仍保留原生VkCmd，未做并发/GPU证明 |
| 初始化失败 | core.constructor/entry、core.init_utils、VMA | initialized/startup_error、常见失败早退、partial cleanup；factory失败返回nullptr+error；recreate仅swapchain/view/semaphore，移除运行路径中的产品targets/samplers创建 |

后端旧 render target/sampler/pipeline recipe 函数定义仍在源码，很多已不在新运行路径调用；不等于已完全删除旧耦合。CMake 已翻转但**链接隔离并未被最终产物证明**。

实施辅助脚本保存在 work：`generate-engine-device.py`、`migrate-engine.py`、`integrate-formal-backend.py`。仅供追查修改来源；**不要原样重跑**，它们会覆盖后续修改，且 migration/集成脚本不是幂等的。

## 4. 本轮 configure/compile 与日志

现有工具链 `work\toolchain\clang64\bin`（clang22.1.7、CMake/Ninja），Release、C++23、无异常/RTTI；Slang `work\slang\bin\slangc.exe`。CMake cache已有prefix等配置，异机不可直接复制此cache。

| 检查 | 实际结果 | 日志 |
|---|---|---|
| 共享GLFW基础 configure | exit0 | `work\shared-glfw-configure.log` |
| 正式第一次 configure | exit1：target_link_libraries plain/keyword混用；已修 | 被最终 `formal-configure.log` 覆盖，诊断在聊天工具记录 |
| 正式最终 configure/generate | exit0 | `work\formal-configure.log` |
| 第一次完整编译尝试 | exit1：u8path deprecated被Werror拒绝；bcrypt先于windows造成类型未定义 | `work\formal-build-first-failure.log` |
| 第二次编译尝试 | exit1：自动clang-format把修正的头文件顺序又排序回去 | `work\formal-build-second-failure.log` |
| 最后一次编译尝试 | **失败**：pipelines.cppm找不到deren::vulkan::hdr_format，行205/219/235/391；日志含ninja stopped | `work\formal-build.log` |
| 最后一次退出码 | 工具调用被用户中断，未收到该次shell最终退出码；不能写成已收集exit1/0 | 同上，失败由实际编译错误与ninja标记确认 |
| 残留构建进程 | 报告前只读查询cmake/ninja/clang++，未返回运行进程 | 聊天工具记录 |
| 源码空白检查 | exit0；只有LF/CRLF提示 | `work\handoff-diff-check-2026-10-04.log` |
| 测试/GPU/最终PE/完整边界门 | 未运行/未完成 | 无本轮通过证据 |

首轮u8path已改u8string path构造；bcrypt依赖顺序已用clang-format off/on保护。最后日志已越过加载器编译进入管线模块。engine_gpu、engine_device、pass模块在最新日志出现构建完成记录，但完整依赖链没有编完；不将单个模块编译当作全程序成功。

下一轮编译复现（须有用户恢复工作授权；当前不要运行）：

```powershell
$env:PATH='D:\deren-workspace\work\toolchain\clang64\bin;' + $env:PATH
& 'D:\deren-workspace\work\toolchain\clang64\bin\cmake.exe' -S 'D:\deren-workspace\deren' -B 'D:\deren-workspace\work\build-release'
& 'D:\deren-workspace\work\toolchain\clang64\bin\cmake.exe' --build 'D:\deren-workspace\work\build-release' --target deren deren_vulkan --parallel 4
```

注意 ALL/构建依赖会执行clang-format（最新日志中出现“Formatting sources with clang-format”）；它是本轮头文件顺序回退的原因，会修改源码。各最终差异应归因核查，不宜把格式化附带变化当成修复。

## 5. 下一位agent需要优先核查的缺陷和风险

以下是未完成工作，不是已经修复的结论；按顺序做最小必要修补。

1. **当前硬编译阻塞：常量模块归属。** pipelines只import engine_gpu，hdr_format等场景/shader常量目前定义在engine_device。应提取无状态布局常量至共同引擎模块并正确re-export，避免引入engine_device↔pipelines循环。保持 `heap_slot_base=16384`、resource stride64、sampler stride32、slots/shader一致。已曾发现生成facade误填8192，父代理已纠正，但需全布局交叉检查。
2. **继续完成编译迁移。** 下一批错误尚未知。检查pass_context的.gpu初始化、recipe参数、old core casts/imports、filters的资源目录接口、frame command借用与owned secondary释放、全部新pure virtual的测试桩。尤其tests/probe_backend.cpp需补vulkan_escape::service override；目前未补、未构建测试target。
3. **图像格式转换不完整。** 后端契约新增了BC/ETC/EAC/ASTC等格式，但engine_gpu::contract_format目前仅手写旧子集。应覆盖真实产品格式或明确拒绝，不把不支持的格式默认成RGBA。检查3D/cube/mips/layers/pitch/span生命周期和MSAA。资源代理说工厂支持扩大格式，仍必须核实调用方映射。
4. **宿主资源目录陈旧记录。** resource_catalog的images/buffers缓存没有随resource_owner释放删除，raw VkImage→RHI object查找可能留下失效指针或复用句柄冲突。这是父代理新增适配层的审查重点，不能只用token保活掩盖每个资源自身的生命周期。
5. **管线状态行为差异。** 原单色make_pipeline convenience默认alpha blend；当前engine_gpu convenience走empty blends会变opaque，尚未补。多目标recipe和单目标透明pass必须保持原blend/depth状态。heap_probe graphics从static状态变factory dynamic，需要draw前明确设置viewport/scissor/cull/depth-write；当前只是wrapper保留viewport/scissor，直接vkCmdBindPipeline路径未必应用它们。
6. **共享sampler堆尚需核实。** 原core create_samplers写六个create infos到sampler grid；现constructor已撤其运行调用，engine_device创建sampler对象但没有等价完整写heap sampler infos。不能只见sampler对象存在就认定shader sampler heap有效；逐项保留filter/address/compare/border/lod及顺序，包括shadow border颜色。
7. **帧/队列错误传播。** native wait_slot现在回填vkWaitSemaphores结果，但engine_device::wait_frame_slot返回void并丢结果；submit_and_wait wrapper把非ok统一映射VK_ERROR_INITIALIZATION_FAILED，会丢DEVICE_LOST等原始结果。recreate失败会置initialized=false，native service也应回传startup_error而非只返回recreated=false。heap_access::ready当前恒true，应按实际状态，transport ok与accepted都需判断。
8. **初始化事务和VMA空哨兵。** 资源代理新增预期失败早退与清理，尚未完整编译/故障测试。VMA staging_upload/host_image_upload缓存耗尽重新create_command_pair失败的两个运行期分支（vma.cppm约851/946）未检测null sentinel；swapchain vkGetSwapchainImages结果未逐项检查。核查partial析构不会wait空device/free未创建对象，不回滚或销毁应用窗口。
9. **契约所有权必须全消费者完成。** object_manager新增token，但只部分缓冲创建点机械接入；逐个核对AS/ray tracing/回读/视图/管线资源。release必须先于最后token释放；image view保image与frame借用规则分别核对。成功加载的DLL包括创建失败路径都pin进程，但core及资源仍主动清理，不热重载。
10. **正式依赖门尚未证明。** 检查CMake目标及BMI图、全engine/chores/main对象的undefined concrete符号、最终EXE后端DLL导入0和DLL导出恰好3个入口。源文件名字/目录换了不能代替该证据；不允许用移动整引擎到DLL或导出concrete core凑清零。
11. **加载器/包正确性。** manifest/compat/shader/GLFW/hash及安全selection路径、损坏/缺包/错工具链/缺入口/工厂拒绝等路径未运行。backend_build_id是版本选择，不是兼容门；兼容门仍为ABI+compat+shader+GLFW。核对runtime标准库递归打包及PE检查成功后才激活selection。GLFW是main前静态导入，缺GLFW可能由Windows loader先报错，应用不能声称全部捕获。
12. **架构与文档收尾。** engine_device是本轮采用的宿主组合适配层，仍保留Vulkan录制耦合和core式便利字段；不是跨API渲染器完成。必须审查是否满足已批准边界、移除陈旧backend recipe/target运行逻辑、更新正式方案/V3结果/README与异机指南，说明首版包限Windows x64。DLL本身不自动提速，当前没有性能结论。

## 6. GPU历史证据及异机验收

`work\capability-probe-2026-10-04-current.log`：RTX4060 Laptop560.70（Vulkan1.3.280）、Intel UHD32.0.101.7085（Vulkan1.4.323）；两卡缺unified_image_layouts、descriptor_heap、shader_untyped_pointers。host_image_copy及GENERAL双向已有，maintenance5已有；maintenance5/extended_flags二选一，不要误称都必须。探针exit2表示所查能力子集0/5，不是程序崩溃。

旧真实产品日志 `work\product-smoke-audit\debug.log` 行37/41明确选中RTX4060，不能再说已证实误选核显。强制独显不能补驱动扩展。正式NVIDIA版本是否支持所有强制扩展尚未实际验证；用户已选择异机验证。

异机应在完整编译和静态门通过后拉取**新提交**，记录commit/配置/资产/shader/toolchain/驱动/物理GPU/扩展/包hash。验证共享GLFW窗口与DPI/最小化恢复、startup失败路径、完整场景画面、validation、资源关闭/引用寿命、队列并发，以及静态参照与动态完整产品性能。旧窗口11/11或CPU17/17不能替代这些门。

## 7. 恢复工作的最低路径

取得用户恢复授权与足够额度后，先核对本报告、80文件snapshot清单、Git diff和最新build日志。优先修常量归属并继续compile-only，逐个消除上述真实缺陷；不要重跑覆盖性的迁移脚本。保留用户“不在本机运行测试”的约束，CPU/窗口/GPU验证安排要按后续授权执行。

编译、完整静态边界与PE检查通过，且文档准确列出未运行的测试与异机验收后，再按用户既有授权本地commit、正常push `origin/codex/dynamic-link-v3` 并核对实时远端SHA。不得将本报告对应的未编译现场包装成已完成的正式动态后端。

## 8. 主仓库接续与同步说明

主仓库 `https://github.com/YzK0741/deren` 的 `master` 已接续方案，获取时最新为 `5eb8eed7e24d4c2f67d6b24419c48fc4930255b7`。近期提交推进图像契约 ABI7、管线契约 ABI8及 pass 迁移；最新提交仍标注 WIP。其 CMake 正式默认仍 STATIC，SHARED 限 spike。上游报告中的验收是其机器记录，本机未复跑。

本地检查点和主仓库无公共祖先（merge-base exit1）。保留 `codex/dynamic-link-v3` 作为旧现场归档，建立 `codex/upstream-sync-2026-10-04` 同步主仓库版本；不将旧半成品强行合并进主仓库，不强推或重写历史。后续开发应从同步分支现有 ABI8 实现继续，原工作区迁移只作对照材料。
