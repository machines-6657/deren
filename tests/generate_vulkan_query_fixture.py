"""Compile production query/selection bodies with CPU-only Vulkan query doubles.

The backend implementation is a module partition. This adapter reads the bodies
verbatim instead of exporting internals or maintaining a second implementation.
The CMake dependency list regenerates the fixture whenever production changes.
Only the Vulkan calls below the code under test are replaced; no loader is linked.
"""
import argparse
from pathlib import Path


def extract(text: str, token: str) -> str:
    start = text.index(token)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    if token.startswith("export struct"):
        end = text.index(";", end) + 1
    return text[start:end].removeprefix("export ")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--vma-source", type=Path, required=True)
    parser.add_argument("--regions-output", type=Path, required=True)
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    tokens = [
        "export struct device_capabilities {",
        "void device_capabilities::query(",
        "export struct queue_family_indices {",
        "bool check_device_extension_support(\n    VkPhysicalDevice physical_device,\n    std::vector<char const*> const& required_extensions) noexcept {",
        "queue_family_indices find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface) noexcept {",
        "VkPhysicalDevice pick_suitable_device(VkInstance instance, VkSurfaceKHR surface) noexcept {",
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n\n".join(extract(source, t) for t in tokens) + "\n", encoding="utf-8")
    vma = args.vma_source.read_text(encoding="utf-8")
    host_upload = extract(vma, "bool vma_allocator::host_image_upload(")
    start = host_upload.index("std::vector<VkMemoryToImageCopyEXT> regions;")
    end = host_upload.index("if (memory_offset > size)", start)
    # Run the actual host-copy region builder; barriers/device calls stay outside
    # this pure CPU test. The fixture supplies only the image description/payload.
    args.regions_output.write_text(
        '#pragma once\n#include "texture_upload_layout.h"\n#include <vector>\n'
        "namespace deren::vulkan {\n"
        "struct upload_fixture_info { uint32_t width, height, array_layers, mip_levels; VkFormat format; };\n"
        "inline std::vector<VkMemoryToImageCopyEXT> host_copy_regions(void const* data, upload_fixture_info const& info) {\n"
        + host_upload[start:end] + "return regions;\n}\n}\n", encoding="utf-8")


if __name__ == "__main__":
    main()
