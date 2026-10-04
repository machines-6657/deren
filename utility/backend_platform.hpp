#pragma once
#include <cstddef>

// Plain TU boundary: Windows/GLFW headers never enter a module's global fragment.
extern "C" {
int deren_backend_executable_directory(char* out, std::size_t capacity);
int deren_backend_glfw_module(char* path, std::size_t capacity, int* major, int* minor, int* revision,
                              char* error, std::size_t error_capacity);
int deren_backend_same_file(char const* actual, char const* expected);
void* deren_backend_open_sha256(char const* path, char* hex, char* error, std::size_t error_capacity);
void deren_backend_close_file(void* handle);
}
