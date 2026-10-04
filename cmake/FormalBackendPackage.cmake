include_guard(GLOBAL)

# Call after targets, their compile options/definitions and shader rules have been defined.
# The loader target is host-only: PUBLIC utility promise glfw, PRIVATE bcrypt on Windows.
function(deren_configure_backend_package)
    cmake_parse_arguments(P "" "HOST;BACKEND;LOADER" "" ${ARGN})
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Formal first-party backend packages currently support Windows x64 only")
    endif()
    foreach(role HOST BACKEND LOADER)
        if(NOT TARGET "${P_${role}}")
            message(FATAL_ERROR "deren_configure_backend_package requires an existing ${role} target")
        endif()
    endforeach()
    get_target_property(_backend_type "${P_BACKEND}" TYPE)
    if(NOT _backend_type STREQUAL "SHARED_LIBRARY")
        message(FATAL_ERROR "The formal backend target must be SHARED")
    endif()
    find_program(DEREN_PACKAGE_POWERSHELL NAMES pwsh powershell REQUIRED)
    get_filename_component(_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_compiler_root "${_compiler_bin}" DIRECTORY)
    find_program(DEREN_PACKAGE_READOBJ NAMES llvm-readobj llvm-readobj.exe
        HINTS "${_compiler_bin}" REQUIRED)
    set(_compiler_target "${CMAKE_CXX_COMPILER_TARGET}")
    if(NOT _compiler_target AND NOT MSVC)
        execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -dumpmachine
            OUTPUT_VARIABLE _compiler_target OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _target_result)
        if(NOT _target_result EQUAL 0)
            message(FATAL_ERROR "Cannot determine the compiler target for backend compatibility identity")
        endif()
    endif()
    if(NOT _compiler_target)
        set(_compiler_target "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}-${CMAKE_SYSTEM_NAME}")
    endif()
    set(_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/backend/$<CONFIG>")
    file(GLOB_RECURSE _contract_files CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/promise/rhi/*.cppm"
        "${CMAKE_CURRENT_SOURCE_DIR}/promise/rhi/*.hpp")
    list(APPEND _contract_files "${CMAKE_CURRENT_SOURCE_DIR}/utility/abi_export.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/vstd/vstd.cppm"
        "${CMAKE_CURRENT_SOURCE_DIR}/vstd/vstd_msvc.cppm")
    file(GLOB_RECURSE _shader_files CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.glsl")
    list(FILTER _shader_files EXCLUDE REGEX "/glsl\\.old/")
    file(GLOB_RECURSE _backend_files CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/*.cpp*"
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/*.hpp")
    # Freeze the actual installed stdlib/CRT files rather than trusting a compiler marketing version.
    set(_stdlib_files)
    foreach(runtime libc++.dll libunwind.dll libstdc++-6.dll libgcc_s_seh-1.dll
                    msvcp140.dll vcruntime140.dll vcruntime140_1.dll)
        if(EXISTS "${_compiler_bin}/${runtime}")
            list(APPEND _stdlib_files "${_compiler_bin}/${runtime}")
        endif()
    endforeach()
    foreach(config_header __config __config_site)
        if(EXISTS "${_compiler_root}/include/c++/v1/${config_header}")
            list(APPEND _stdlib_files "${_compiler_root}/include/c++/v1/${config_header}")
        endif()
    endforeach()
    # CPU/shader layouts and heap bindings participate even though they are host-side modules.
    file(GLOB_RECURSE _cpu_interface_files CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/gltf_loader/*.cppm"
        "${CMAKE_CURRENT_SOURCE_DIR}/gltf_loader/*.cppm"
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/runtime/*.cppm"
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/core/descriptor_heap/*.cppm")
    list(APPEND _shader_files ${_cpu_interface_files})
    # Bracket quoting preserves spaces/semicolons; generator expressions resolve actual config options.
    set(_input "set(source_root [==[${CMAKE_CURRENT_SOURCE_DIR}]==])\n")
    foreach(pair IN ITEMS contract_files shader_files backend_files stdlib_files)
        string(APPEND _input "set(${pair} [==[${_${pair}}]==])\n")
    endforeach()
    string(APPEND _input
        "set(compiler [==[${CMAKE_CXX_COMPILER}]==])\n"
        "set(compiler_id [==[${CMAKE_CXX_COMPILER_ID}]==])\n"
        "set(compiler_version [==[${CMAKE_CXX_COMPILER_VERSION}]==])\n"
        "set(compiler_target [==[${_compiler_target}]==])\n"
        "set(configuration [==[$<CONFIG>]==])\n"
        "set(architecture [==[${CMAKE_SYSTEM_PROCESSOR};${CMAKE_CXX_COMPILER_ARCHITECTURE_ID};${CMAKE_SIZEOF_VOID_P}]==])\n"
        "set(cxx_standard [==[${CMAKE_CXX_STANDARD}]==])\n"
        "set(global_cxx_flags [==[${CMAKE_CXX_FLAGS}]==])\n"
        "set(config_cxx_flags [==[$<$<CONFIG:Debug>:${CMAKE_CXX_FLAGS_DEBUG}>$<$<CONFIG:Release>:${CMAKE_CXX_FLAGS_RELEASE}>$<$<CONFIG:RelWithDebInfo>:${CMAKE_CXX_FLAGS_RELWITHDEBINFO}>$<$<CONFIG:MinSizeRel>:${CMAKE_CXX_FLAGS_MINSIZEREL}>]==])\n"
        "set(host_options [==[$<TARGET_GENEX_EVAL:${P_HOST},$<TARGET_PROPERTY:${P_HOST},COMPILE_OPTIONS>>]==])\n"
        "set(backend_options [==[$<TARGET_GENEX_EVAL:${P_BACKEND},$<TARGET_PROPERTY:${P_BACKEND},COMPILE_OPTIONS>>]==])\n"
        "set(loader_options [==[$<TARGET_GENEX_EVAL:${P_LOADER},$<TARGET_PROPERTY:${P_LOADER},COMPILE_OPTIONS>>]==])\n"
        "set(host_definitions [==[$<TARGET_GENEX_EVAL:${P_HOST},$<TARGET_PROPERTY:${P_HOST},COMPILE_DEFINITIONS>>]==])\n"
        "set(backend_definitions [==[$<TARGET_GENEX_EVAL:${P_BACKEND},$<TARGET_PROPERTY:${P_BACKEND},COMPILE_DEFINITIONS>>]==])\n"
        "set(msvc_runtime [==[${CMAKE_MSVC_RUNTIME_LIBRARY}]==])\n"
        "set(implicit_link_libraries [==[${CMAKE_CXX_IMPLICIT_LINK_LIBRARIES}]==])\n"
        "set(implicit_link_directories [==[${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES}]==])\n"
        "set(glfw_file [==[$<TARGET_FILE:deren_glfw_shared>]==])\n"
        "set(glfw_header [==[${VR_GLFW_INCLUDE_DIR}/GLFW/glfw3.h]==])\n"
        "set(slang_compiler [==[${VR_SLANGC_EXECUTABLE}]==])\n"
        "set(shader_entries [==[${VR_SLANG_SOURCES}]==])\n"
        "set(shader_compile_contract [==[${DEREN_SHADER_COMPILE_CONTRACT}]==])\n"
        "set(output_directory [==[${_generated}]==])\n")
    if(NOT DEREN_SHADER_COMPILE_CONTRACT)
        message(FATAL_ERROR "Set DEREN_SHADER_COMPILE_CONTRACT to the exact Slang compile argument list")
    endif()
    file(GENERATE OUTPUT "${_generated}/identity-input.cmake" CONTENT "${_input}")
    add_custom_target(deren_backend_identity
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${_generated}/identity-input.cmake"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GenerateBackendIdentity.cmake"
        BYPRODUCTS "${_generated}/deren_backend_identity.hpp" "${_generated}/backend.manifest.seed.json"
        VERBATIM)
    add_dependencies("${P_LOADER}" deren_backend_identity)
    target_include_directories("${P_LOADER}" PRIVATE "${_generated}")
    get_filename_component(_glfw_bin "${VR_GLFW_RUNTIME_LIBRARY}" DIRECTORY)
    add_custom_target(deren_release_package ALL
        COMMAND "${CMAKE_COMMAND}" "-DSEED=${_generated}/backend.manifest.seed.json"
            "-DBACKEND=$<TARGET_FILE:${P_BACKEND}>" "-DHOST=$<TARGET_FILE:${P_HOST}>"
            "-DGLFW=$<TARGET_FILE:deren_glfw_shared>" "-DSHADERS=${VR_SHADER_OUT_DIR}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/StageBackendPackage.cmake"
        COMMAND "${DEREN_PACKAGE_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/windows/package_dynamic_backend.ps1"
            -HostFile "$<TARGET_FILE:${P_HOST}>" -BackendFile "$<TARGET_FILE:${P_BACKEND}>"
            -SeedFile "${_generated}/backend.manifest.seed.json" -ReadObj "${DEREN_PACKAGE_READOBJ}"
            -RuntimeDirectories "${_compiler_bin}|${_glfw_bin}"
        DEPENDS "${P_HOST}" "${P_BACKEND}" deren_backend_identity shaders
        VERBATIM)
    # Explicit package target controls build ordering without giving the EXE a backend link/BMI edge.
endfunction()
