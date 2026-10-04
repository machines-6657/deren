# 共享 GLFW 与异机验收准备实施计划

基点：ec39712cfb4c05d398a58a9c48a62b4a067d0bc1，分支 codex/dynamic-link-v3。

目标：应用、后端及 ImGui 使用同一份共享 GLFW，自动准备 Windows 运行库，推送可在另一台机器构建/验收的源码与检查入口。

设计：采用用户选择的共享 GLFW；应用负责初始化、窗口/事件及终止，后端借用 GLFWwindow 并调用 GLFW Vulkan surface 接口。保留三个后端入口及现有 ABI；此次共享依赖改变不改变窗口指针语义或接口布局。完整资源契约/动态后端翻转仍单独记录进度。

用户最新要求：先不在本机测试，写完 push，用户拉到其他机器测试后反馈。本轮不运行 CTest、窗口探针或 GPU 产品测试；只做必要 configure/compile、导入表和代码检查。之前窗口探针 11/11 是旧记录，不作为本批源码重新测试的结果。V3 的“先本机 GPU 基线再写代码”顺序按本次用户要求调整，最终 GPU 验收门不取消。

## 任务与交付

- [ ] 统一共享依赖：cmake/SharedGlfw.cmake 定义 imported SHARED GLFW；CMakeLists.txt 的既有 glfw interface 对所有消费者传播共享导入库及 Windows GLFW_DLL，拒绝以静态库代替。
- [ ] 运行库准备：Windows 在 EXE 同目录复制同一份 glfw3.dll；显式构建主程序或后端也运行准备步骤，不依赖开发机 PATH。
- [ ] 异机窗口检查：tests/shared_glfw 的 EXE/DLL 使用产品同一 GLFW target；检查初始化、窗口、模块/函数地址、错误及终止状态，不纳入默认 headless CTest。
- [ ] 交接文档：记录已完成范围、旧 GPU 证据、必需能力、复现命令、待用户执行的窗口/GPU验收及未完成的正式后端阶段。
- [ ] 构建与推送：configure/compile，检查实际 PE 导入，git diff --check；按用户要求不跑测试；本地提交并正常推送 origin/codex/dynamic-link-v3，不强推或合并上游。

## 本轮约束与审查重点

1. 所有 GLFW 消费者必须用同一 imported target，包括后端、runtime 与 ImGui；不能仅改 main。
2. MSVC 配套导入库是 glfw3dll.lib，不能误取静态 glfw3.lib；MinGW 明确选择 DLL 导入库。
3. 打包不能让后端目录和 EXE目录存在两个不同 GLFW；核对实际导入/运行模块。
4. GLFW 生命周期由应用统一管理；共享依赖不解决任意错误窗口指针、回调覆盖、线程违规或 GPU 同步。
5. 缺库的进入 main 前失败由 Windows loader 报告；不虚构应用已处理该失败。

实施方式：当前 D 盘已有干净的专用修复分支，继续使用它，构建/日志保留 D:\deren-workspace\work。用户已经授权写代码和 push，且要求异机测试；不另建 C 盘工作区，不以技能默认测试流程重复请求授权。每个阶段检查额度，剩余 <=5% 时停止修改，写交接报告并停工。

## 最新授权与实施账本

用户已明确本轮完成正式动态后端，GPU验收放异机。原“只共享GLFW”范围已被覆盖。ABI7资源工厂、上下文加载器/manifest、Vulkan专用帧/heap服务和引擎资源所有权正在接线。不得把未编译/未验收的工作记为完成。

- 基点 ec39712；用户禁止本机运行测试，保留编译、静态边界与PE检查。
- 核心布局 heap_slot_base=16384, resource stride64, sampler stride32 必须保持shader一致。
- render target和pipeline recipes属于engine，设备/队列/分配属于backend；宿主无concrete core/BMI/import-library依赖。
- 当前需完整编译诊断、修复与异机验收；未提交。
- 额度<=5%停写、停止构建/提交/push，保留现场交接。
