#version 460

// Textured N64 combiner, color + alpha.  Mirrors shaders/opengl/default.shader.glsl: general (a-b)*c+d per channel,
// inter-cycle WRAP for 2-cycle, final WRAP(-0.51,1.51)+clamp(0,1), then the gated alpha-test discards.
//
// Two-cycle texel swap: in cycle 1 the N64 texel pipeline shifts, so TEXEL0 reads tile1 and TEXEL1 reads tile0.  We
// reproduce that by passing the effective (t0,t1) pair per cycle -- (gTexel0,gTexel1) in cycle 0, swapped in cycle 1 --
// rather than special-casing inside the selector.

layout (location = 0) in vec2 vTexCoord0;
layout (location = 1) in vec2 vTexCoord1;
layout (location = 2) in vec4 vInput1; // .a carries the combiner alpha input when opt_alpha
layout (location = 3) in vec4 vInput2;
layout (location = 4) in vec4 vInput3;
layout (location = 5) in vec4 vInput4;
layout (location = 0) out vec4 fColor;

// Texture units.  deko3d keeps sampler bindings in a separate id space from uniform buffers, so binding 0 here does
// not collide with the CombinerUbo block below.
layout (binding = 0) uniform sampler2D uTex0;
layout (binding = 1) uniform sampler2D uTex1;

layout (std140, binding = 0) uniform CombinerUbo {
    ivec4 uC0Color; // cycle 0, color: (a,b,c,d)
    ivec4 uC0Alpha; // cycle 0, alpha
    ivec4 uC1Color; // cycle 1, color
    ivec4 uC1Alpha; // cycle 1, alpha
    int uNumInputs;
    int uDo2cyc;
    int uOptAlpha; // 1 -> compute alpha via the alpha column; else alpha is forced opaque
    int uOptFog;
    int uOptGrayscale;
    int uUsedTex0; // sample uTex0 only when 1 (the unused slot still has a valid handle bound)
    int uUsedTex1;
    int uOptTextureEdge;    // Cutout alpha test: a > 0.19 ? 1.0 : discard
    int uOptAlphaThreshold; // a < 8/256 ? discard
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

// Color source for a combiner item.  t0/t1 are the effective texels for the cycle being evaluated.
vec3 ccSel(int item, vec3 combined, vec4 t0, vec4 t1) {
    if (item == SHADER_INPUT_1) {
        return vInput1.rgb;
    }

    if (item == SHADER_INPUT_2) {
        return vInput2.rgb;
    }

    if (item == SHADER_INPUT_3) {
        return vInput3.rgb;
    }

    if (item == SHADER_INPUT_4) {
        return vInput4.rgb;
    }

    if (item == SHADER_TEXEL0) {
        return t0.rgb;
    }

    if (item == SHADER_TEXEL0A) {
        return vec3(t0.a);
    }

    if (item == SHADER_TEXEL1) {
        return t1.rgb;
    }

    if (item == SHADER_TEXEL1A) {
        return vec3(t1.a);
    }

    if (item == SHADER_1) {
        return vec3(1.0);
    }

    if (item == SHADER_COMBINED) {
        return combined;
    }

    return vec3(0.0); // SHADER_0/unmapped
}

// Alpha source for a combiner item.  Both TEXEL0 and TEXEL0A resolve to the texel's alpha in the alpha column.
float acSel(int item, float combined, vec4 t0, vec4 t1) {
    if (item == SHADER_INPUT_1) {
        return vInput1.a;
    }

    if (item == SHADER_INPUT_2) {
        return vInput2.a;
    }

    if (item == SHADER_INPUT_3) {
        return vInput3.a;
    }

    if (item == SHADER_INPUT_4) {
        return vInput4.a;
    }

    if (item == SHADER_TEXEL0 || item == SHADER_TEXEL0A) {
        return t0.a;
    }

    if (item == SHADER_TEXEL1 || item == SHADER_TEXEL1A) {
        return t1.a;
    }

    if (item == SHADER_1) {
        return 1.0;
    }

    if (item == SHADER_COMBINED) {
        return combined;
    }

    return 0.0; // SHADER_0/unmapped
}

vec3 wrap3(vec3 x, float lo, float hi) {
    return mod(x - lo, hi - lo) + lo;
}

float wrap1(float x, float lo, float hi) {
    return mod(x - lo, hi - lo) + lo;
}

vec3 evalCycleColor(ivec4 s, vec3 combined, vec4 t0, vec4 t1) {
    vec3 a = ccSel(s.x, combined, t0, t1);
    vec3 b = ccSel(s.y, combined, t0, t1);
    vec3 c = ccSel(s.z, combined, t0, t1);
    vec3 d = ccSel(s.w, combined, t0, t1);
    return (a - b) * c + d;
}

float evalCycleAlpha(ivec4 s, float combined, vec4 t0, vec4 t1) {
    float a = acSel(s.x, combined, t0, t1);
    float b = acSel(s.y, combined, t0, t1);
    float c = acSel(s.z, combined, t0, t1);
    float d = acSel(s.w, combined, t0, t1);
    return (a - b) * c + d;
}

void main() {
    // Sampling is gated on uniform conditions, so it is well-defined even though the unused unit still has a handle.
    vec4 t0 = uUsedTex0 == 1 ? texture(uTex0, vTexCoord0) : vec4(0.0);
    vec4 t1 = uUsedTex1 == 1 ? texture(uTex1, vTexCoord1) : vec4(0.0);

    vec3 cLane = evalCycleColor(uC0Color, vec3(0.0), t0, t1);
    float aLane = uOptAlpha == 1 ? evalCycleAlpha(uC0Alpha, 0.0, t0, t1) : 1.0;

    if (uDo2cyc == 1) {
        cLane = wrap3(cLane, uC1Color.z == SHADER_COMBINED ? -1.01 : -0.51,
                      uC1Color.z == SHADER_COMBINED ? 1.01 : 1.51);
        if (uOptAlpha == 1) {
            aLane = wrap1(aLane, uC1Alpha.z == SHADER_COMBINED ? -1.01 : -0.51,
                          uC1Alpha.z == SHADER_COMBINED ? 1.01 : 1.51);
        }

        // Cycle 1: TEXEL0 reads tile1, TEXEL1 reads tile0.
        cLane = evalCycleColor(uC1Color, cLane, t1, t0);
        if (uOptAlpha == 1) {
            aLane = evalCycleAlpha(uC1Alpha, aLane, t1, t0);
        }
    }

    vec4 texel = vec4(cLane, aLane);
    texel = vec4(wrap3(texel.rgb, -0.51, 1.51), wrap1(texel.a, -0.51, 1.51));
    texel = clamp(texel, 0.0, 1.0);

    if (uOptTextureEdge == 1) {
        if (texel.a > 0.19) {
            texel.a = 1.0;
        } else {
            discard;
        }
    }

    if (uOptAlphaThreshold == 1) {
        if (texel.a < 8.0 / 256.0) {
            discard;
        }
    }

    fColor = texel;
}