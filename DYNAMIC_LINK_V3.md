# 动态后端方案 v3：评审结论与可执行顺序

日期：2026-10-03。输入是 `deren.zip` 的 ABI 6 源码，GitHub `8eaaa6a4663ca886b1d85a37467a2224ffa3e0bf` 仅用于恢复压缩包缺失的文件。V2 的实验记录保留；历史“实测”不是本次验证。

## 结论

保留单后端、三个 C 入口、资源后端所有权、应用窗口所有权、能力缺失具名失败、不做热重载和降级。V2 用链接符号衡量后端依赖比数 `vk*` 合理，但它的翻转条件和门工具都不足以证明独立 DLL 已成立。

### 必须修正的设计

1. **运行期解析意味着 EXE 对后端 DLL 的静态导入为零。** 三个 `deren_*` 是 DLL 导出，不是 EXE 导入。正常的系统/标准库导入另列；留在 Vulkan pass 的系统 Vulkan 调用与后端 C++ 依赖分别核查。
2. **资源、core 成员和 pass 对具体后端的依赖必须一起清零，才能翻转。** `②a` 完成不足以翻转；其余 25 个 core 成员不能挂在翻转后的分支上。目录搬动和符号内联不能算完成迁移。
3. **CMake 的 PRIVATE 不保证静态库不传递链接依赖。** 静态库的依赖可以通过 `$<LINK_ONLY:...>` 传播。最后必须彻底去掉引擎对 `deren_vulkan` target/import library 的链接，契约/BMI 由独立 target 提供，以真实导入表验收。
4. **这是同工具链的 C++ ABI，不是通用 C ABI。** `extern "C"` 只稳定三个入口名；对象仍通过虚表和 `std::span` 交换。冻结编译器、标准库、架构、CRT、结构布局、异常/RTTI选项，合同 ABI 跳号与工具链指纹一起检查。当前版本号不会自动发现同为 ABI 6 的不兼容构建。
5. **保留 `query_extension` 单一能力入口。** 当前没有已实现的 `deren_ext_*` C 表，取消代码注释里的额外导出要求。明确不能因此服务非 C++ 宿主。
6. **共享 GLFW 指针不能解决两个静态 GLFW 实例的问题。** 最终 descriptor 明确传 HWND（Windows）或具名平台窗口结构；应用负责 GLFW，后端用 Vulkan 平台 surface API。framebuffer 尺寸通过契约通知，不能继续调用 DLL 自己的 GLFW 全局状态。
7. **FreeLibrary 卡住的原因尚未证明。** 进程生命周期内保持模块加载符合“不热重载”约束；但 `detach()` 是保持引用的策略，不是根因修复。所有资源仍必须先销毁，再销毁 core；窗口最后销毁。失败路径也要防止卸载仍在运行的模块。
8. **Sanitizer 结论限于测过的工具链与配置。** 一次 Windows clang/lld 失败不能推出所有平台无法覆盖 DLL，更不能无条件对所有非 MSVC 构建关闭插桩。产品覆盖与 headless 覆盖分别报告，未跑的平台标未验。

## 可靠的门

`check_backend_boundary.py` 必须满足：

- 比较符号集合，出现新符号即失败，不能用删一个加一个抵消。
- `--update` 只能接受当前集合是旧集合子集且 owning STL 不增长；失败不写文件。
- 缺少基线默认失败；首次建账必须显式 `--initialize`，已有账不能重新初始化。
- 正向依赖之外检查后端对引擎的反向依赖；对主程序对象和 `chores` 也核查。
- 过期半构建不允许更新基线；时间检查只是保守诊断，成功验收仍需完整干净构建。
- `--require-zero` 单独表达翻转门，普通棘轮通过不等于翻转完成。
- 输出机器可读报告、joiners/leavers及对象来源。历史 baseline 77 不因文档的 66 自动改写。

通用行为门保留：完整构建、全部 CTest、逐场景 validation、固定输入/工具链/驱动上的渲染基线、SPIR-V 文件清单和摘要、A5 屏障逐字段一致。缺资产、基线不存在或结果不同均不算通过；更换机器必须先确定比较范围，不能静默 `-Update`。

## 执行顺序

| 阶段 | 交付与出口 |
|---|---|
| P0：先让证据可靠 | 恢复缺失源码，适配唯一 `rhi::create_info`；修边界棘轮和加载器输入；真实回归测试、接 CTest/CI、从干净构建重新测量 |
| P1：图像/视图 | 完整 format/usage/subresource desc；后端按内容和尺寸/格式共同去重；视图保持图像引用；所有创建、读回和释放路径先有真设备测试，再迁调用点 |
| P2：剩余资源/录制 | sampler/shader/pipeline desc、descriptor heap 真实能力、二级命令缓冲归后端；先抽出四个引擎管线配方，再删除具体后端对象访问 |
| P3：core与窗口 | runtime 持契约对象；帧同步、GPU timing、extent等按语义迁移，不作机械改名；窗口归应用，后端移除 GLFW |
| P4：翻转 | 各引擎消费者和反向依赖均为零；SHARED窄导出三个名字；动态加载/握手/后端删除器；EXE没有后端 DLL 导入和具体后端链接依赖 |
| P5：完成解耦 | runtime无原生Vulkan类型；escape限定Vulkan pass；全行为门和失败路径通过后才认定完成 |

P0 可以独立验收。P1–P5 必须先有可运行的产品基线；如果本机不支持项目强制扩展，则停在具名失败和已验证的改动，不能更改渲染基线或加入降级凑绿。

驱动解锁按具体扩展和功能位验收，不能只看 Vulkan 版本：必须核查 `VK_KHR_unified_image_layouts`、`VK_EXT_descriptor_heap`、`VK_KHR_shader_untyped_pointers`、heap 所需的 maintenance5/extended_flags，以及 `VK_EXT_host_image_copy` 和 GENERAL 上传/读回布局。2026-10-03 实测本机 Intel UHD 已报告 Vulkan 1.4.323，却仍缺前三项；RTX 4060 Laptop 的 560.70 驱动也缺前三项。设备查询通过后仍要完成真实创建、渲染及 validation 门。

## 图像阶段的额外约束

`image::make_view` 应返回可管理的契约视图，不能返回裸 `void*` 作为拥有对象。视图需要覆盖 aspect、类型（2D/数组/cube）、mip/layer范围，并持有所引用图像的后端引用。上传数据需要明确每个mip/layer的offset、row pitch、slice pitch；单个无布局span不能表达任意图像上传。图像内容摘要必须连同format、尺寸、层数、mip数和使用约束构成缓存键。`image_format::depth` 是后端选择的角色，不能用于解释CPU字节。

资源必须在 core 前释放的义务用拥有类型/成员析构顺序落实，增加“创建资源后立即关闭”和创建中途失败门；靠一条注释提醒不是所有权方案。

## 本次验证

最终命令、结果、产物位置和未通过的门记录在 `DYNAMIC_LINK_V3_RESULT.md`。这个文件只有在命令实际完成后更新。
