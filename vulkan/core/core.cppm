// ============================================================================
// module: deren.vulkan.core
// module version: 0.24.0  (independent of the app version in CMakeLists project(VERSION))
//
// GPU scaffolding: instance / device / swapchain / VMA / pipeline / descriptor
// plumbing (core.vma / core.pipeline / core.filter / core.init_utils submodules
// are part of this unit). Standalone Vulkan wrapper; depends on VMA + utility,
// with the struct-fill conventions coming from vulkan.constant_init.
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================

// THE PRIMARY INTERFACE IS DELIBERATELY THIS SMALL. It re-exports the declarations and imports NO
// implementation partition: under -Werror that import is an error in clang 22 (see core.declarations.cppm's
// header), and it is unnecessary - CMake compiles every partition listed in the module's file set, so
// the definitions in them are archived and the linker finds them. `import deren.vulkan.core;` behaves exactly
// as it did before the split, for all twelve importers.

export module deren.vulkan.core;
export import :declarations;