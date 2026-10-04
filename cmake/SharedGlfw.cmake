# 应用和后端必须共享同一份 GLFW 状态；不要在各个 PE 中静态链接 GLFW。
set(VR_GLFW_ROOT "" CACHE PATH
    "Shared GLFW distribution root (include/ and DLL/import library); required for MSVC")
if(MSVC AND NOT VR_GLFW_ROOT)
    message(FATAL_ERROR "MSVC requires VR_GLFW_ROOT pointing at a shared GLFW distribution")
endif()

set(_deren_glfw_hints)
set(_deren_glfw_search_options)
if(VR_GLFW_ROOT)
    list(APPEND _deren_glfw_hints "${VR_GLFW_ROOT}")
    # 指定发行包时不混入系统的其他 GLFW 版本。
    list(APPEND _deren_glfw_search_options NO_DEFAULT_PATH)
endif()
find_path(VR_GLFW_INCLUDE_DIR NAMES GLFW/glfw3.h
    HINTS ${_deren_glfw_hints} PATH_SUFFIXES include
    ${_deren_glfw_search_options} REQUIRED)

add_library(deren_glfw_shared SHARED IMPORTED GLOBAL)
set_target_properties(deren_glfw_shared PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${VR_GLFW_INCLUDE_DIR}")
if(WIN32)
    if(MSVC)
        set(_deren_glfw_import_names glfw3dll.lib)
    else()
        set(_deren_glfw_import_names libglfw3.dll.a libglfw3dll.a)
    endif()
    find_file(VR_GLFW_IMPORT_LIBRARY NAMES ${_deren_glfw_import_names}
        HINTS ${_deren_glfw_hints}
        PATH_SUFFIXES lib lib-vc2022 lib-mingw-w64
        ${_deren_glfw_search_options} REQUIRED)
    get_filename_component(_deren_glfw_import_dir "${VR_GLFW_IMPORT_LIBRARY}" DIRECTORY)
    find_file(VR_GLFW_RUNTIME_LIBRARY NAMES glfw3.dll
        HINTS ${_deren_glfw_hints} "${_deren_glfw_import_dir}"
        PATH_SUFFIXES bin lib-vc2022 lib-mingw-w64
        ${_deren_glfw_search_options} REQUIRED)
    set_target_properties(deren_glfw_shared PROPERTIES
        IMPORTED_IMPLIB "${VR_GLFW_IMPORT_LIBRARY}"
        IMPORTED_LOCATION "${VR_GLFW_RUNTIME_LIBRARY}"
        INTERFACE_COMPILE_DEFINITIONS GLFW_DLL)
else()
    find_library(VR_GLFW_RUNTIME_LIBRARY NAMES glfw glfw3
        HINTS ${_deren_glfw_hints} PATH_SUFFIXES lib lib64
        ${_deren_glfw_search_options} REQUIRED)
    if(VR_GLFW_RUNTIME_LIBRARY MATCHES "\\.a$")
        message(FATAL_ERROR "Shared GLFW is required; found static archive: ${VR_GLFW_RUNTIME_LIBRARY}")
    endif()
    set_target_properties(deren_glfw_shared PROPERTIES
        IMPORTED_LOCATION "${VR_GLFW_RUNTIME_LIBRARY}")
endif()

# 保留已有消费者使用的 target 名称，使共享模式覆盖 backend/runtime/ImGui。
add_library(glfw INTERFACE)
target_link_libraries(glfw INTERFACE deren_glfw_shared)
message(STATUS "Shared GLFW runtime: ${VR_GLFW_RUNTIME_LIBRARY}")
