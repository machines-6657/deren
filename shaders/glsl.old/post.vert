#version 450

/**
 * @file shaders/post.vert
 * @brief Fullscreen triangle for every post-process pass (post.frag and fxaa.frag).
 * @ingroup shaders
 *
 * No vertex buffer: the triangle is synthesized from gl_VertexIndex, which covers the whole screen
 * with 3 vertices and avoids the diagonal seam a two-triangle quad can show with some filters. The
 * UV follows the framebuffer orientation, so v_uv = (0,0) is the top-left texel of the source image.
 *
 * Shared by both pipelines that render fullscreen passes: the bloom/composite stages
 * (runtime::make_post_pipeline) and FXAA (runtime::make_fxaa_pipeline).
 */
layout(location = 0) out vec2 v_uv;

/**
 * @brief emit the fullscreen triangle: position from gl_VertexIndex, v_uv in [0,1]^2
 */
void main() {
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
