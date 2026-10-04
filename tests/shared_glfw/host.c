#include "probe.h"
#include <stdio.h>

static int checks;
static int failures;
#define CHECK(expr, label) do { \
    ++checks; \
    if (!(expr)) { ++failures; printf("FAIL: %s\n", label); } \
    else printf("PASS: %s\n", label); \
} while (0)

int main(void) {
    GLFWwindow* window = NULL;
    probe_snapshot snapshot = {0};
    int width = 0, height = 0;
    int major = 0, minor = 0, revision = 0;
    char module_path[MAX_PATH] = {0};
    HMODULE host_module = NULL;

    CHECK(probe_inspect(NULL, &snapshot) == 0, "null window rejected");
    if (!glfwInit()) {
        const char* reason = NULL;
        int code = glfwGetError(&reason);
        printf("glfwInit failed: %d %s\n", code, reason ? reason : "");
        return 2;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window = glfwCreateWindow(160, 120, "deren shared GLFW probe", NULL, NULL);
    CHECK(window != NULL, "host creates hidden no-API window");
    if (!window) goto cleanup;

    CHECK(probe_inspect(window, NULL) == 0, "null output rejected");
    CHECK(probe_inspect(window, &snapshot) == 1, "DLL uses host-initialized GLFW without its own init");
    glfwGetFramebufferSize(window, &width, &height);
    CHECK(snapshot.hwnd != NULL && snapshot.hwnd == glfwGetWin32Window(window), "host and DLL obtain identical HWND");
    CHECK(width > 0 && height > 0 && snapshot.width == width && snapshot.height == height,
          "host and DLL obtain identical framebuffer pixels");
    glfwGetVersion(&major, &minor, &revision);
    CHECK(snapshot.major == major && snapshot.minor == minor && snapshot.revision == revision,
          "host and DLL report identical GLFW version");
    CHECK(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)glfwGetWin32Window, &host_module) &&
          host_module == snapshot.glfw_module,
          "host and DLL functions belong to identical loaded module");
    CHECK(snapshot.native_getter == glfwGetWin32Window,
          "host and DLL resolve identical native getter address");
    if (host_module) GetModuleFileNameA(host_module, module_path, MAX_PATH);
    printf("GLFW=%d.%d.%d module=%s framebuffer=%dx%d\n", major, minor, revision, module_path, width, height);

    (void)glfwGetError(NULL);
    probe_invalid_hint();
    CHECK(glfwGetError(NULL) == GLFW_INVALID_ENUM, "host reads error produced through DLL");

cleanup:
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    probe_invalid_hint();
    CHECK(glfwGetError(NULL) == GLFW_NOT_INITIALIZED, "DLL observes host termination");
    printf("CHECKS=%d FAILURES=%d\n", checks, failures);
    return failures ? 1 : 0;
}
