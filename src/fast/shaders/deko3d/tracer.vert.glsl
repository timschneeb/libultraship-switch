#version 460

// No vertex buffer, no attributes, no uniforms -- the whole point is to prove the uam -> .dksh -> romfs -> loader ->
// bindShaders -> draw -> present chain in isolation.  Emits an R/G/B gradient so the result is unambiguously not the
// magenta clear and proves varying interpolation.

layout(location = 0) out vec3 vColor;

void main() {
    vec2 positions[3] = vec2[](
        vec2( 0.0,  0.6),
        vec2(-0.6, -0.6),
        vec2( 0.6, -0.6)
    );
    vec3 colors[3] = vec3[](
        vec3(1.0, 0.0, 0.0),
        vec3(0.0, 1.0, 0.0),
        vec3(0.0, 0.0, 1.0)
    );

    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
    vColor = colors[gl_VertexID];
}
