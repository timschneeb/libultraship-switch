#version 460

// Textured combiner column.  Vertex layout matches the interpreter's packing with usedTextures != {0,0}:
//   aVtxPos    vec4  @ byte 0
//   aTexCoord0 vec2        (present iff usedTextures[0]; any clampS/clampT floats follow but are not bound here)
//   aTexCoord1 vec2        (present iff usedTextures[1])
//   aInput1..N vec4 each,  N = numInputs  (.a carries the combiner alpha input when opt_alpha; bound 4x32 then,
//                                            3x32 otherwise so .a defaults and is left unread)
//
// Attribute locations are fixed regardless of which textures are live.  The backend binds unused texcoord slots at
// byte offset 0 (a dead varying) so the input locations never shift between draws -- this is what lets a single
// compiled variant serve tex0-only/tex1-only/ both.

layout (location = 0) in vec4 aVtxPos;
layout (location = 1) in vec2 aTexCoord0;
layout (location = 2) in vec2 aTexCoord1;
layout (location = 3) in vec4 aInput1;
layout (location = 4) in vec4 aInput2;
layout (location = 5) in vec4 aInput3;
layout (location = 6) in vec4 aInput4;

layout (location = 0) out vec2 vTexCoord0;
layout (location = 1) out vec2 vTexCoord1;
layout (location = 2) out vec4 vInput1;
layout (location = 3) out vec4 vInput2;
layout (location = 4) out vec4 vInput3;
layout (location = 5) out vec4 vInput4;

void main() {
    gl_Position = aVtxPos;
    vTexCoord0 = aTexCoord0;
    vTexCoord1 = aTexCoord1;
    vInput1 = aInput1;
    vInput2 = aInput2;
    vInput3 = aInput3;
    vInput4 = aInput4;
}