#version 460

// First real uber-variant: untextured, no alpha, 1 input, 1 cycle.  Vertex layout matches the interpreter's base
// packing exactly:
//   aVtxPos  vec4  @ byte 0   (x, y, z, w -- already in clip space, no MVP)
//   aInput1  vec3  @ byte 16  (the single shade/combiner input, no alpha)
// stride = 28 bytes.
//
// Eventually, the interpreter's buf_vbo will feed these same attributes, unchanged.

layout(location = 0) in vec4 aVtxPos;
layout(location = 1) in vec3 aInput1;

layout(location = 0) out vec3 vColor;

void main() {
    gl_Position = aVtxPos;
    vColor = aInput1;
}
