#include "probe.h"

/* 临时探针不初始化/终止 GLFW：只检查宿主创建的窗口与共享库状态。 */
PROBE_API int probe_inspect(GLFWwindow* window, probe_snapshot* out) {
    if (!window || !out) return 0;
    out->hwnd = glfwGetWin32Window(window);
    glfwGetFramebufferSize(window, &out->width, &out->height);
    glfwGetVersion(&out->major, &out->minor, &out->revision);
    out->native_getter = glfwGetWin32Window;
    out->glfw_module = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)out->native_getter, &out->glfw_module)) return 0;
    return out->hwnd != NULL;
}

/* 非法 hint 不改变窗口配置；用它验证同一线程的 GLFW 错误状态确实共享。 */
PROBE_API void probe_invalid_hint(void) {
    glfwWindowHint(-1, 0);
}
