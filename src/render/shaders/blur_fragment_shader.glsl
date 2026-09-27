// Fragment highp is optional in GLSL ES 1.00; as Chromium's
// gles2_cmd_copy_texture_chromium.cc, fall back to mediump without it.
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec2 uStep;
uniform float uCenterWeight;
uniform vec2 uTaps[15];
uniform int uTapCount;

void main() {
    vec4 color = texture2D(uTexture, vTexCoord) * uCenterWeight;
    for (int i = 0; i < 15; ++i) {
        if (i >= uTapCount)
            break;
        vec2 delta = uTaps[i].x * uStep;
        color += (texture2D(uTexture, vTexCoord + delta)
                + texture2D(uTexture, vTexCoord - delta)) * uTaps[i].y;
    }
    gl_FragColor = color;
}
