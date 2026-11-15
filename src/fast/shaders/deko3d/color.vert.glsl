#version 460

// Untextured combiner column.  Vertex layout matches the interpreter's packing with usedTextures=false:
//   aVtxPos  vec4  @ byte 0
//   aInput1..N vec3 each, N = numInputs (fog/grayscale floats, if packed, sit between pos and inputs and are simply
//                                        not bound as attributes).
//
// The backend binds only the first numInputs inputs; the rest read defaults and are never selected.

layout (location = 0) in vec4 aVtxPos;
layout (location = 1) in vec3 aInput1;
layout (location = 2) in vec3 aInput2;
layout (location = 3) in vec3 aInput3;
layout (location = 4) in vec3 aInput4;

layout (location = 0) out vec3 vInput1;
layout (location = 1) out vec3 vInput2;
layout (location = 2) out vec3 vInput3;
layout (location = 3) out vec3 vInput4;

void main() {
    gl_Position = aVtxPos;
    vInput1 = aInput1;
    vInput2 = aInput2;
    vInput3 = aInput3;
    vInput4 = aInput4;
}