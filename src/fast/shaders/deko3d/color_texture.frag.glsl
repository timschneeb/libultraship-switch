#version 460

// Textured N64 combiner, color channel.  Extends color.frag.glsl's untextured column with TEXEL0/TEXEL1 sampling;
// the (a-b)*c+d/2-cycle WRAP/final clamp structure is identical -- only the ccSel sources differ.

layout (location = 0) in vec2 vTexCoord0;
layout (location = 1) in vec2 vTexCoord1;
layout (location = 2) in vec3 vInput1;
layout (location = 3) in vec3 vInput2;
layout (location = 4) in vec3 vInput3;
layout (location = 5) in vec3 vInput4;
layout (location = 0) out vec4 fColor;

// Texture units.  Bound via cmdbuf.bindTextures(DkStage_Fragment, 0, {handle0, handle1}); deko3d keeps sampler
// bindings in a separate id space from uniform buffers, so binding 0 here does not collide with the CombinerUbo block
// below.
layout (binding = 0) uniform sampler2D uTex0;
layout (binding = 1) uniform sampler2D uTex1;

layout (std140, binding = 0) uniform CombinerUbo {
    ivec4 uC0Color; // cycle 0, color: (a,b,c,d)
    ivec4 uC0Alpha; // cycle 0, alpha (unused for now)
    ivec4 uC1Color; // cycle 1, color
    ivec4 uC1Alpha; // cycle 1, alpha (unused for now)
    int uNumInputs;
    int uDo2cyc;
    int uOptAlpha;
    int uOptFog;
    int uOptGrayscale;
    int uUsedTex0; // sample uTex0 only when 1 (the unused slot still has a valid handle bound)
    int uUsedTex1;
};

const int SHADER_0 = 0;
const int SHADER_INPUT_1 = 1;
const int SHADER_INPUT_2 = 2;
const int SHADER_INPUT_3 = 3;
const int SHADER_INPUT_4 = 4;
const int SHADER_TEXEL0 = 8;
const int SHADER_TEXEL0A = 9;
const int SHADER_TEXEL1 = 10;
const int SHADER_TEXEL1A = 11;
const int SHADER_1 = 12;
const int SHADER_COMBINED = 13;

vec4 gTexel0;
vec4 gTexel1;

vec3 ccSel(int item, vec3 combined) {
    if (item == SHADER_INPUT_1) {
        return vInput1;
    }

    if (item == SHADER_INPUT_2) {
        return vInput2;
    }

    if (item == SHADER_INPUT_3) {
        return vInput3;
    }

    if (item == SHADER_INPUT_4) {
        return vInput4;
    }

    if (item == SHADER_TEXEL0) {
        return gTexel0.rgb;
    }

    if (item == SHADER_TEXEL0A) {
        return vec3(gTexel0.a);
    }

    if (item == SHADER_TEXEL1) {
        return gTexel1.rgb;
    }

    if (item == SHADER_TEXEL1A) {
        return vec3(gTexel1.a);
    }

    if (item == SHADER_1) {
        return vec3(1.0);
    }

    if (item == SHADER_COMBINED) {
        return combined;
    }

    return vec3(0.0); // SHADER_0/unmapped
}

vec3 wrap3(vec3 x, float lo, float hi) {
    return mod(x - lo, hi - lo) + lo;
}

vec3 evalCycle(ivec4 s, vec3 combined) {
    vec3 a = ccSel(s.x, combined);
    vec3 b = ccSel(s.y, combined);
    vec3 c = ccSel(s.z, combined);
    vec3 d = ccSel(s.w, combined);
    return (a - b) * c + d;
}

void main() {
    // Sampling is gated on uniform conditions, so it is well-defined even though the unused unit still has a handle.
    gTexel0 = uUsedTex0 == 1 ? texture(uTex0, vTexCoord0) : vec4(0.0);
    gTexel1 = uUsedTex1 == 1 ? texture(uTex1, vTexCoord1) : vec4(0.0);

    vec3 texel = evalCycle(uC0Color, vec3(0.0));

    if (uDo2cyc == 1) {
        if (uC1Color.z == SHADER_COMBINED) {
            texel = wrap3(texel, -1.01, 1.01);
        } else {
            texel = wrap3(texel, -0.51, 1.51);
        }

        texel = evalCycle(uC1Color, texel);
    }

    texel = wrap3(texel, -0.51, 1.51);
    texel = clamp(texel, 0.0, 1.0);
    fColor = vec4(texel, 1.0);
}
