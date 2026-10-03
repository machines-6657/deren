# 动态后端 v3 验证结果（P0）

日期：2026-10-03。对应方案见 [DYNAMIC_LINK_V3.md](DYNAMIC_LINK_V3.md)；本文只记实际执行过的命令、结果、产物位置和未通过的门。

## 输入与环境

| 项 | 值 |
|---|---|
| 源码 | `C:\Users\Kevin\Downloads\deren.zip`（1.41 GB，ABI 6，含 build-release-clang64 缓存） |
| 补缺来源 | https://github.com/YzK0741/deren @ `8eaaa6a4663ca886b1d85a37467a2224ffa3e0bf`（2026-10-03，接口仍为 ABI 1），仅用于恢复 31 个缺失文件（含 `main.cpp`），清单在 `D:\deren-workspace\work\restored_files.json` |
| 工具链 | `D:\deren-workspace\work\toolchain\clang64`（MSYS2 clang64 共 56 包 176.2 MiB，由 `work\bootstrap_toolchain.py` 按 `work\packages.txt` 下载解包，清单 `work\toolchain_manifest.json`） |
| 着色器编译 | slang 2026.18.2（sha256 `747602ae…` 校验通过）解包于 `work\slang` |
| 构建目录 | `D:\deren-workspace\work\build-release`（Ninja，Release，`-DVR_NATIVE_ARCH=OFF`） |
| 主机 | Windows 11 x64；NVIDIA GeForce RTX 4060 Laptop GPU，驱动 560.70，Vulkan 1.3.280；无预装 Vulkan SDK / clang64 |

## P0 执行记录（按顺序）

1. **门回归测试先行**：`python -m unittest discover -s tests -p test_backend_boundary.py -v`，用真实编译产物归档覆盖：同数替换符号必须失败、`--update` 拒绝越权且失败不改文件、缺基线失败、`--initialize` 不可重初始化、反向依赖、过期归档拒绝、`--require-zero` 需要应用证据、报告与基线不得同路径。最终 **23 个测试全过**（`work\python-final.log`）。
2. **`scripts/check_backend_boundary.py` 重写**：joiners 即失败（同数替换不可抵消）；`--update` 仅在当前集合 ⊆ 旧集合且 owning STL 不增时棘轮下调，失败不写文件；无基线默认失败，首次必须显式 `--initialize`；量完引擎归档再量 `libchores.a` 与 `main.cpp` 目标文件（`--app-object` 供非标准布局），主程序不能借引擎归档蒙混；反向依赖（后端未定义 ∩ 引擎定义 − 后端自定义）即失败；消费者比后端旧 5 分钟以上拒绝测量；基线写入走临时文件 + `os.replace`；`--report` 输出机器可读测量（consumers/joiners/leavers/symbol_sources/application_evidence）。兼容保留的 `--warn` 可将普通检查失败降为报告，但不能放行 `--update`、`--initialize` 或 `--require-zero`；CI 不传 `--warn`。
3. **加载器输入校验**：`utility/dynamic_link.cppm` 的 `load()` 在补后缀前先拒绝空串与含 NUL 的 `string_view`（系统会在 NUL 处截断路径导致静默加载错误目标）；`tests/test_dynamic_link.cpp` 增加两组回归；CTest `test_dynamic_link` Passed。
4. **能力入口收敛**：`promise/rhi/rhi.extension.cppm` 取消 `deren_ext_<ability>_v1` C 函数表要求，一致性门改为逐置位检查 `query_extension()` 返回可用对象；边界不承诺服务非 C++ 宿主。
5. **应用窗口所有权**：`main.cpp` 新增 `application_window`（RAII，先于 runtime 构造、晚于 runtime 析构，`GLFW_INCLUDE_NONE`），`core_create_info` 改为统一的 `promise::rhi::create_info`，后端经 `native_window` 只借用窗口。
6. **接线**：`CMakeLists.txt` 注册 `test_backend_boundary`（CTest 第 1 项，独立 test-run 工作目录）与 `check-backend-boundary` target（`DEPENDS deren`，先完整构建再测量）；`.github/workflows/ci.yml` 在全量构建后新增 `Check backend boundary ratchet`；`scripts/windows/check_render.ps1` 纠正基线目录环境变量的说明注释（实际代码此前已使用 `DEREN_BASELINE_DIR`）。CI 配置已接入，远程 GitHub CI 尚未运行。
7. **完整构建**：clang64 工具链 configure + `cmake --build … --parallel 1`（资源所限降并发），全部 C++ 模块与 14 个测试可执行链接成功（`work\build-full-serial.log`、`work\build-final.log`）。
8. **CTest**：`ctest --test-dir work/build-release` → **15/15 Passed，19.96 s**（`work\ctest-release.log`）。
9. **边界测量**：`check-backend-boundary` target → 后端 `libderen_vulkan.a` 1167 个定义符号；**66 个跨界符号 / 180 处引用 / 3 个消费者**（引擎归档 77 成员 + `libchores.a` + `main.cpp.obj`），0 个携带 owning STL。分类：24 core 成员、24 `vk_*` RAII 句柄、8 descriptor_heap、4 init_utils、3 vma_allocator、2 pipeline、1 init/guard；区域：runtime 92、pass 64、other 24（`work\boundary-gate.log`、`work\build-release\backend_boundary_report.json`）。
10. **基线棘轮**：确认与 V2 末次记录（66）一致后执行 `--update`，`scripts/backend_boundary_baseline.mingw.json` 由 77 棘轮至 **66**（git diff 可查）；复验输出 `OK: 66 symbols, baseline 66, 180 reference sites`。
11. **产品冒烟**：`work\product-smoke\smoke.toml`（DamagedHelmet.gltf，128×128，关闭验证层，capture 模式 1 帧后截图退出）→ **具名失败**：`VK_KHR_unified_image_layouts is required but not supported by the device`（`init_device_and_queue`，debug.log 完整打印了设备能力：descriptor heap 不可用、unified layout NOT available）；进程 exit code `-1073740791`（0xC0000409）。未修改渲染基线，未添加降级路径（符合 V3 的具名失败约束）。

12. **翻转门行为验证**：`--require-zero` 在 66 符号的当前树上按设计拒绝：`FAIL: flip gate requires zero backend dependencies AND main/chores evidence (found 66 symbols)`，exit 1——翻转门不会被普通棘轮通过所冒充。
13. **P1 盘点文档事实核查**：`work\p1-image-view-inventory.md`（66 符号映射 + 图像面盘点 + 6 步建议）经独立抽查 60+ 个文件:行号锚点：0 处实质错误，仅 3 处 ±1~2 行容差内漂移；行号请当"区间锚点"用（先到附近再按函数名确认）。

## 未通过的门 / 阻塞

- **渲染基线**：本机 NVIDIA 560.70（Vulkan 1.3.280）同时缺少 `VK_KHR_unified_image_layouts`、`VK_EXT_descriptor_heap` 和 `VK_KHR_shader_untyped_pointers`，原冒烟首先停在 unified layouts。Intel UHD 虽支持 Vulkan 1.4.323，也缺这三项，因此换用核显不能解锁。升级必须按实际扩展和功能位核验，不能把“Vulkan 1.4”当作充分条件。在此之前 P1–P5 缺少产品行为基线，不得宣称推进。
- **翻转门**：`--require-zero` 未过（66 ≠ 0）。当前 66 符号与 V2 末次记录一致，说明 P0 期间迁移无回退、也无净新增，但资源/core/pass 的具体后端依赖仍待 P1–P3 清零。
- **环境注意**：在裸 PATH 下直接运行 `check_backend_boundary.py` 会命中 `C:\msys64\ucrt64\bin\nm.EXE`（GNU nm 读不了 clang64 的 COFF 目标，报 file format not recognized）。请走 `cmake --build … --target check-backend-boundary`，或先把 `D:\deren-workspace\work\toolchain\clang64\bin` 前置到 PATH（脚本优先选 `llvm-nm`）。

## 接手续验（2026-10-03）

- `cmake --build D:/deren-workspace/work/build-release --parallel 1`：exit 0，`ninja: no work to do`；这是当前树的增量确认，首次完整编译证据仍见上面的构建日志。
- `ctest --test-dir D:/deren-workspace/work/build-release --output-on-failure`：exit 0，15/15，19.90 s；其中边界脚本实际执行 23 个回归测试（17.136 s，OK），日志 `work/build-release/Testing/Temporary/LastTest.log`。
- 独立末次审查发现 `--app-object` 可把后端归档冒充应用。补四个真实归档回归，原实现四例均 exit 0（RED），修复后拒绝后端归档作为任何消费者，并按 realpath/normcase/文件身份排除引擎与后端的路径别名及硬链接。修复后完整 CTest **15/15，22.98 s**；边界回归 **27/27**。RED 日志 `work/boundary-app-evidence-red.log`、`boundary-app-alias-red.log`；最终 CTest 输出 `work/ctest-resume-final.log`，详细日志已另存 `work/ctest-resume-audit.log`（初次23例）和当前 `work/build-release/Testing/Temporary/LastTest.log`（27例）。
- `check-backend-boundary`：exit 0，66/66、180 sites、3 consumers、0 owning STL、0 reverse、0 stale，应用证据完整。旧基线到新基线只删除 11 个符号，没有新增；未再次更新基线。
- `clang-format-check`：exit 0；`git diff --check` 无空白错误。
- 独立设备查询：`work/capability_probe.cpp` 编译 exit 0；查询 exit 2 表示 5 个 Vulkan 适配器中 0 个满足所查能力子集（含两块真实 GPU 和三个 Microsoft 映射适配器），不是查询程序崩溃。完整结果 `work/capability-probe.log`。这里只查询设备，不创建逻辑设备或渲染，不等于产品验收。
- 同配置重新运行 `deren.exe --config D:/deren-workspace/work/product-smoke/smoke.toml --capture-frames 1`：exit -1073740791（0xC0000409），再次在 `init_device_and_queue` 报 unified layouts 不支持；完整日志 `work/product-smoke-audit/debug.log` 与 `exit_code.txt`。渲染结果仍未产生，验证层仍关闭，原有基线未写入。
- [NVIDIA 官方 Vulkan 驱动页](https://developer.nvidia.com/vulkan-driver) 当前下载栏列 Windows **597.11** 开发测试驱动（正文摘要仍写 596.99，页面本身有版本文字不一致）；支持列表含 RTX 4060 Laptop。发行记录：573.38（2025-06-08）新增 unified layouts，582.30（2026-01-23）新增 descriptor heap。官方页面快照保存于 `work/references/nvidia-vulkan-driver.html`。驱动仍未下载/安装，候选驱动尚未在本机验证。

## 恢复时改动清单

接手时 `git status`：10 个修改（`.github/workflows/ci.yml`、`CMakeLists.txt`、`DYNAMIC_LINK_V2.md`、`main.cpp`、`promise/rhi/rhi.extension.cppm`、`scripts/backend_boundary_baseline.mingw.json`、`scripts/check_backend_boundary.py`、`scripts/windows/check_render.ps1`、`tests/test_dynamic_link.cpp`、`utility/dynamic_link.cppm`）+ 4 个未跟踪项（`DYNAMIC_LINK_V3.md`、本文、`docs/superpowers/`、`tests/test_backend_boundary.py`）。暂存区和 stash 为空。交接补丁 `D:\deren-workspace\work\uncommitted.patch` 实测 76,506 bytes，反向应用检查通过；正式交接正文另写 75,794 bytes，且与粘贴附件并非逐字一致。原报告和补丁保留，不覆盖；本轮追加的核验记录与文档修正以 Git 提交为准。

## 本地交付

分支 `codex/dynamic-link-v3`，起始快照 `9a650f3`。代码按逻辑保存为：

- `11fd95d`：边界门、归档身份校验、27 个回归、CMake/CI 接线及 77→66 基线，同批提交。
- `948cc1a`：空路径/NUL 输入校验及真实 DLL 回归。
- `ab14ca6`：恢复的 main 适配 ABI 6、应用窗口所有权、单一能力入口说明。

V3 方案、实施计划、结果记录和渲染脚本注释另按文档批次保存。独立审查无未解决 Critical/Important，报告 `work/p0-final-review.md`；P0 的本地编译与回归验收成立，GPU 渲染、远程 CI、最终 DLL 翻转和 P1–P5 均不在此完成声明内。
