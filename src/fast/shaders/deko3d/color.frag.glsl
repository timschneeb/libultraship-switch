#version 460

layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 fColor;

// Prove the deko3d uniform-buffer path.  Identity tint => no visual change; a non-identity tint proves the bound value
// actually reaches the fragment stage.
layout(std140, binding = 0) uniform CombinerUbo {
    vec4 uTint;
};

void main() {
    fColor = vec4(vColor, 1.0) * uTint;
}