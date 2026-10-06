#version 460

layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aUv;
layout (location = 2) in vec4 aColor;

layout (location = 0) out vec2 vUv;
layout (location = 1) out vec4 vColor;

layout (std140, binding = 0) uniform ProjUbo {
    mat4 uProj;
};

void main() {
    vUv = aUv;
    vColor = aColor;
    gl_Position = uProj * vec4(aPos, 0.0, 1.0);
}
