// The GRAPHICS half of the heap-native probe: a fullscreen triangle built from the vertex index alone.
//
// There is deliberately NO vertex buffer: what this probe tests is the FRAGMENT stage's access to the heap (see
// heap_probe.frag), and a vertex buffer would add a second thing that could be wrong. It is a separate file from
// heap_probe.comp because a graphics pipeline needs its own stage pair, and the compute probe's job (a
// layout-less, push-data-fed, heap-flagged pipeline) is already done.
#version 460

layout(location = 0) out vec2 uv;

void main() {
    // The qualifier is LEADING (`const vec2`), which the file mask_bake.comp already records: glslc rejects a
    // trailing `const` on a declaration (`vec2 const x`) with a syntax error rather than a helpful message.
    const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    const vec2 p = positions[gl_VertexIndex];
    uv = (p + vec2(1.0)) * 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
