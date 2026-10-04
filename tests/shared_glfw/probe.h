#pragma once

#include <windows.h>
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#ifdef PROBE_BUILD
#define PROBE_API __declspec(dllexport)
#else
#define PROBE_API __declspec(dllimport)
#endif

typedef struct probe_snapshot {
    HWND hwnd;
    int width;
    int height;
    int major;
    int minor;
    int revision;
    HMODULE glfw_module;
    HWND (*native_getter)(GLFWwindow*);
} probe_snapshot;

PROBE_API int probe_inspect(GLFWwindow* window, probe_snapshot* out);
PROBE_API void probe_invalid_hint(void);
