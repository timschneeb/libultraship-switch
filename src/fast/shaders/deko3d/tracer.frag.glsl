#version 460

// Just emit the interpolated vertex color.

layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 fColor;

void main() {
    fColor = vec4(vColor, 1.0);
}
