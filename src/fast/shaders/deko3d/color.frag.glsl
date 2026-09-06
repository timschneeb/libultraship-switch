#version 460

// Untextured N64 combiner, color channel.  Mirrors shaders/opengl/default.shader.glsl:
// general (a-b)*c+d form (do_single/multiply/mix are reductions of it), inter-cycle WRAP for 2cyc, final
// WRAP(-0.51,1.51) + clamp(0,1).

layout (location = 0) in vec3 vInput1;
layout (location = 1) in vec3 vInput2;
layout (location = 2) in vec3 vInput3;
layout (location = 3) in vec3 vInput4;
layout (location = 0) out vec4 fColor;

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
};

const int SHADER_0 = 0;
const int SHADER_INPUT_1 = 1;
const int SHADER_INPUT_2 = 2;
const int SHADER_INPUT_3 = 3;
const int SHADER_INPUT_4 = 4;
const int SHADER_1 = 12;
const int SHADER_COMBINED = 13;

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