#version 460

// Trivial combiner result -- output the single input directly (the numInputs=1, no-texture, no-alpha, 1-cycle case
// where the combiner reduces to "input1").

layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 fColor;

void main() {
    fColor = vec4(vColor, 1.0);
}
