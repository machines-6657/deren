# 动态后端 v3 实施计划

目标：修正 V2 的不可证验收逻辑，在 ABI 6 源码上先完成 P0；产品行为基线允许时按 V3 的 P1–P5 继续。

执行方式：本会话直接实现，保持原始 ZIP、上游 commit 和本地起始快照。

- [x] 恢复缺失文件并适配 main 的创建参数/应用窗口所有权；用完整构建验证，不覆盖已迁移的 ABI 6 文件。
- [x] 在 `tests/test_backend_boundary.py` 用真实 C 归档测试 CLI：新增/替换符号、更新拒绝且文件不变、缺基线、反向依赖、过期归档、零边界与报告。
- [x] 先运行测试确认失败；修改 `scripts/check_backend_boundary.py` 的集合棘轮、更新和测量逻辑；重跑全部 Python 测试。
- [x] 在既有 `test_dynamic_link` 加空路径/NUL路径回归；先确认错误，再修 `utility/dynamic_link.cppm`；重跑 CTest。
- [x] 接 CMake 和 CI；工具放 `D:/deren-workspace/work/toolchain`，构建产物放 work/build-*，从干净构建测边界，保留原基线。
- [x] 验证产品启动、必需扩展、资源/渲染基线。若具名失败则登记阻塞，P1–P5不宣称完成。（具名失败：VK_KHR_unified_image_layouts，见 DYNAMIC_LINK_V3_RESULT.md）
- [x] 自查 diff、生成补丁与结果报告；说明源代码来源、命令/exit code及尚未通过的门。（补丁：work\uncommitted.patch；报告：DYNAMIC_LINK_V3_RESULT.md）
- [x] 接手后独立复审，修复后端归档/归档别名冒充应用证据；四个新增回归均 RED→GREEN，完整 CTest 15/15、边界回归27/27，真实边界66/66保持不变。

关注点：同数替换符号不可抵消；更新失败不能毁基线；主程序不能绕过引擎归档检查；warn不应放行更新；资料中的历史门不能充当本次证据。
