# 技术缺陷修复记录 · 2026-10-04

本次修复四项可复现的历史结构缺陷，不进行 V3 接口迁移，也不添加能力降级。
相关旧逻辑在接手快照 `9a650f3` 与本次修改前 `8a75911` 中一致；这只能证明接手前已有，不能据此判断上游最早引入日期。

## 缺陷、触发条件和修复

| 编号 | 触发条件和原行为 | 根因 | 修复后的行为 |
|---|---|---|---|
| F1 | 设备支持 descriptor heap 和 untyped pointers，但缺少 micromap 或光追扩展；untyped 功能被误判不可用 | 独立着色器功能被挂在可选 micromap 查询节点之后 | 在独立功能链尾部追加 untyped 节点；创建前检查 heap 与着色器依赖均可用 |
| F2 | 缺 mesh/heap/host copy/micromap 中任一属性节点，或 micromap 功能关闭；RT handle size、AS scratch alignment 读回为零 | 属性链把后续节点固定挂在可能不可达的前置节点后面 | 每项属性按自己的条件追加到有效尾部；缺少无关节点不会截断后续查询 |
| F3 | 第一块 GPU 满足 Vulkan 1.3、队列和 swapchain 条件，但缺强制渲染能力；第二块 GPU 满足要求 | 选卡先返回第一块，创建阶段才发现缺能力并终止 | 对每个候选查询完整能力；选卡与创建共用强制能力检查；逐设备打印缺失项并继续尝试下一块 |
| F4 | 上传 BC/ETC/EAC/ASTC 的紧密打包纹理，尤其多 mip、多 layer、小于压缩块的尾部 mip | 每块字节数被当成每像素字节数，另有 BC4、ETC/EAC 分组错误 | 使用真实块宽高和块字节数，向上取整计算块数；总大小校验与 host-copy mip 偏移共用计算函数 |

F3 检查 dynamicRendering、descriptor heap 及扩展依赖、shaderUntypedPointers、unifiedImageLayouts，以及 host image copy 的 GENERAL 上传/读回布局。mesh 与光追仍按现有策略作为可选能力。

F4 示例：4×4 BC1/BC4 均为 8 字节，原算法分别算成 128/256 字节。5×7 BC1、2 layers、3 mips 总计 96 字节，各 mip 起点为 0、64、80。ETC2 RGB/RGB+A1 与 EAC R11 每块 8 字节；ETC2 RGBA8 与 EAC R11G11 每块 16 字节。ASTC 根据各规格的块尺寸计数，每块 16 字节。

上传布局另增加零尺寸、零 layer/mip、超出完整 mip 链、未知格式及整数溢出检查。短数据仍被拒绝；空渲染目标保留现有无初始数据行为。

## 修改位置

- `vulkan/core/init_utils/init_utils.cppm`：功能/属性查询链与候选 GPU 检查。
- `vulkan/core/core.constructor.cppm`：与选卡共用检查，确保 heap 着色器依赖被启用。
- `vulkan/core/vma/texture_upload_layout.h`、`vma.cppm`：后端内部块布局计算与两处上传调用。
- `tests/test_vulkan_capabilities.cpp`、`test_texture_upload_layout.cpp`、`generate_vulkan_query_fixture.py`：CPU 回归及实际源码适配。
- `CMakeLists.txt`、`.github/workflows/ci.yml`：生成依赖、测试注册、CI 构建列表及格式检查。

## 验证

先运行旧逻辑：能力回归 29 个检查中 18 个失败；纹理回归 66 个检查中 34 个失败，均为预期缺陷表现。修复后补充边界用例，能力回归 37/37、纹理布局回归 78/78 通过。

本次 Windows / clang 22 / Release 实际结果：

- 完整 Release 构建：退出码 0。
- CTest：17/17 通过，退出码 0。
- `check-backend-boundary`：66 symbols、180 reference sites、3 consumers、0 owning STL；基线未修改，退出码 0。
- `clang-format-check`、`git diff --check`：退出码 0。

最后一次边界复查曾按时间戳规则拒绝主程序对象、chores 和引擎归档；重建主程序/chores，并从当前对象重新生成引擎归档后再验证，未放宽检查或更新基线。

可重复命令：

```powershell
$env:PATH = 'D:/deren-workspace/work/toolchain/clang64/bin;' + $env:PATH
cmake --build D:/deren-workspace/work/build-release --parallel 1
ctest --test-dir D:/deren-workspace/work/build-release --output-on-failure
cmake --build D:/deren-workspace/work/build-release --target check-backend-boundary
cmake --build D:/deren-workspace/work/build-release --target clang-format-check
```

测试适配器读取实际模块分区中的查询/选卡实现以及 host-copy 区域构造循环，替换底层 Vulkan 查询入口，不复制业务算法、不链接 Vulkan loader、不创建设备。纹理大小直接调用产品内部头文件。生成器依赖函数签名和区域标记，相关源码重命名时必须同步调整；标记缺失会使构建失败。

本机日志保存在 `D:/deren-workspace/work/structural-audit/` 的 `capabilities-red.log`、`texture-layout-red.log`、`build-fixes.log`、`ctest-fixes.log`、`boundary-fixes.log`、`format-fixes.log`，不作为云端可用文件宣称。

独立审查发现并修复了生成测试头文件未声明自身依赖的问题。该审查随后因额度限制中断，未形成完整独立审查结论；最终差异由实施者复核。

## 验证边界与后续缺陷

以上是 CPU 逻辑回归和构建验证，不是 GPU 渲染验收。现有驱动缺少项目强制扩展，真实启动/渲染仍受阻；本次未安装或修改驱动。没有证据表明当前材质在使用压缩格式，因此不把 F4 说成现有启动失败的原因。

后续待单独复现和修复的线索：LINEAR 图像直接写入未考虑 rowPitch/subresource offset；VMA 队列互斥未覆盖其他共享 graphics queue 的提交点。这些不计入本次已修复清单。

云端提交使用修复分支保存本地源码状态；目标上游写权限不足时推送当前账号 fork。云端提交不等于上游合并，也不等于 GitHub Actions 已通过。
