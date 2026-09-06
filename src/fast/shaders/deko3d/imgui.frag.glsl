#version 460

layout (location = 0) in vec2 vUv;
layout (location = 1) in vec4 vColor;
layout (location = 0) out vec4 fColor;

layout (binding = 0) uniform sampler2D uTex;

void main() {
    fColor = vColor * texture(uTex, vUv);
}
