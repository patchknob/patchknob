//----------------------------------------------------------------------------
//  src/engine/rack/nanovg.h
//
//  Inert NanoVG API surface for Rack SDK bridge builds.  Plugin headers often
//  define custom widgets whose draw() bodies call NanoVG; the bridge never
//  runs that code (panels are pre-rendered to textures), but it must compile.
//  Every function is an inline no-op returning zeros.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_RACK_NANOVG_STUB_H
#define PATCHKNOB_RACK_NANOVG_STUB_H

struct NVGcontext;

struct NVGcolor {
    union {
        float rgba[4];
        struct { float r, g, b, a; };
    };
};

struct NVGpaint {
    float xform[6];
    float extent[2];
    float radius;
    float feather;
    NVGcolor innerColor;
    NVGcolor outerColor;
    int image;
};

struct NVGglyphPosition {
    const char* str;
    float x;
    float minx, maxx;
};

struct NVGtextRow {
    const char* start;
    const char* end;
    const char* next;
    float width;
    float minx, maxx;
};

enum NVGwinding { NVG_CCW = 1, NVG_CW = 2 };
enum NVGsolidity { NVG_SOLID = 1, NVG_HOLE = 2 };
enum NVGlineCap { NVG_BUTT, NVG_ROUND, NVG_SQUARE, NVG_BEVEL, NVG_MITER };
enum NVGalign {
    NVG_ALIGN_LEFT = 1 << 0, NVG_ALIGN_CENTER = 1 << 1, NVG_ALIGN_RIGHT = 1 << 2,
    NVG_ALIGN_TOP = 1 << 3, NVG_ALIGN_MIDDLE = 1 << 4, NVG_ALIGN_BOTTOM = 1 << 5,
    NVG_ALIGN_BASELINE = 1 << 6
};
enum NVGblendFactor {
    NVG_ZERO = 1 << 0, NVG_ONE = 1 << 1, NVG_SRC_COLOR = 1 << 2,
    NVG_ONE_MINUS_SRC_COLOR = 1 << 3, NVG_DST_COLOR = 1 << 4,
    NVG_ONE_MINUS_DST_COLOR = 1 << 5, NVG_SRC_ALPHA = 1 << 6,
    NVG_ONE_MINUS_SRC_ALPHA = 1 << 7, NVG_DST_ALPHA = 1 << 8,
    NVG_ONE_MINUS_DST_ALPHA = 1 << 9, NVG_SRC_ALPHA_SATURATE = 1 << 10
};
enum NVGcompositeOperation {
    NVG_SOURCE_OVER, NVG_SOURCE_IN, NVG_SOURCE_OUT, NVG_ATOP,
    NVG_DESTINATION_OVER, NVG_DESTINATION_IN, NVG_DESTINATION_OUT,
    NVG_DESTINATION_ATOP, NVG_LIGHTER, NVG_COPY, NVG_XOR
};
enum NVGimageFlags {
    NVG_IMAGE_GENERATE_MIPMAPS = 1 << 0, NVG_IMAGE_REPEATX = 1 << 1,
    NVG_IMAGE_REPEATY = 1 << 2, NVG_IMAGE_FLIPY = 1 << 3,
    NVG_IMAGE_PREMULTIPLIED = 1 << 4, NVG_IMAGE_NEAREST = 1 << 5
};

inline NVGcolor nvgRGBAf(float r, float g, float b, float a) {
    NVGcolor color; color.r = r; color.g = g; color.b = b; color.a = a; return color;
}
inline NVGcolor nvgRGBf(float r, float g, float b) { return nvgRGBAf(r, g, b, 1.f); }
inline NVGcolor nvgRGBA(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    return nvgRGBAf(r / 255.f, g / 255.f, b / 255.f, a / 255.f);
}
inline NVGcolor nvgRGB(unsigned char r, unsigned char g, unsigned char b) { return nvgRGBA(r, g, b, 255); }
inline NVGcolor nvgLerpRGBA(NVGcolor c0, NVGcolor c1, float u) {
    NVGcolor color;
    for (int i = 0; i < 4; ++i) color.rgba[i] = c0.rgba[i] + (c1.rgba[i] - c0.rgba[i]) * u;
    return color;
}
inline NVGcolor nvgTransRGBA(NVGcolor c, unsigned char a) { c.a = a / 255.f; return c; }
inline NVGcolor nvgTransRGBAf(NVGcolor c, float a) { c.a = a; return c; }
inline NVGcolor nvgHSLA(float, float, float l, unsigned char a) { return nvgRGBAf(l, l, l, a / 255.f); }
inline NVGcolor nvgHSL(float h, float s, float l) { return nvgHSLA(h, s, l, 255); }

inline void nvgBeginFrame(NVGcontext*, float, float, float) {}
inline void nvgEndFrame(NVGcontext*) {}
inline void nvgCancelFrame(NVGcontext*) {}
inline void nvgSave(NVGcontext*) {}
inline void nvgRestore(NVGcontext*) {}
inline void nvgReset(NVGcontext*) {}
inline void nvgGlobalAlpha(NVGcontext*, float) {}
inline void nvgGlobalTint(NVGcontext*, NVGcolor) {}
inline void nvgGlobalCompositeOperation(NVGcontext*, int) {}
inline void nvgGlobalCompositeBlendFunc(NVGcontext*, int, int) {}
inline void nvgGlobalCompositeBlendFuncSeparate(NVGcontext*, int, int, int, int) {}
inline void nvgShapeAntiAlias(NVGcontext*, int) {}
inline void nvgStrokeColor(NVGcontext*, NVGcolor) {}
inline void nvgStrokePaint(NVGcontext*, NVGpaint) {}
inline void nvgFillColor(NVGcontext*, NVGcolor) {}
inline void nvgFillPaint(NVGcontext*, NVGpaint) {}
inline void nvgMiterLimit(NVGcontext*, float) {}
inline void nvgStrokeWidth(NVGcontext*, float) {}
inline void nvgLineCap(NVGcontext*, int) {}
inline void nvgLineJoin(NVGcontext*, int) {}
inline void nvgTranslate(NVGcontext*, float, float) {}
inline void nvgRotate(NVGcontext*, float) {}
inline void nvgSkewX(NVGcontext*, float) {}
inline void nvgSkewY(NVGcontext*, float) {}
inline void nvgScale(NVGcontext*, float, float) {}
inline void nvgTransform(NVGcontext*, float, float, float, float, float, float) {}
inline void nvgResetTransform(NVGcontext*) {}
inline void nvgCurrentTransform(NVGcontext*, float* xform) {
    if (xform) { xform[0] = 1; xform[1] = 0; xform[2] = 0; xform[3] = 1; xform[4] = 0; xform[5] = 0; }
}
inline void nvgScissor(NVGcontext*, float, float, float, float) {}
inline void nvgIntersectScissor(NVGcontext*, float, float, float, float) {}
inline void nvgResetScissor(NVGcontext*) {}
inline void nvgBeginPath(NVGcontext*) {}
inline void nvgMoveTo(NVGcontext*, float, float) {}
inline void nvgLineTo(NVGcontext*, float, float) {}
inline void nvgBezierTo(NVGcontext*, float, float, float, float, float, float) {}
inline void nvgQuadTo(NVGcontext*, float, float, float, float) {}
inline void nvgArcTo(NVGcontext*, float, float, float, float, float) {}
inline void nvgClosePath(NVGcontext*) {}
inline void nvgPathWinding(NVGcontext*, int) {}
inline void nvgArc(NVGcontext*, float, float, float, float, float, int) {}
inline void nvgRect(NVGcontext*, float, float, float, float) {}
inline void nvgRoundedRect(NVGcontext*, float, float, float, float, float) {}
inline void nvgRoundedRectVarying(NVGcontext*, float, float, float, float, float, float, float, float) {}
inline void nvgEllipse(NVGcontext*, float, float, float, float) {}
inline void nvgCircle(NVGcontext*, float, float, float) {}
inline void nvgFill(NVGcontext*) {}
inline void nvgStroke(NVGcontext*) {}

inline NVGpaint nvgLinearGradient(NVGcontext*, float, float, float, float, NVGcolor icol, NVGcolor ocol) {
    NVGpaint paint = {}; paint.innerColor = icol; paint.outerColor = ocol; return paint;
}
inline NVGpaint nvgBoxGradient(NVGcontext*, float, float, float, float, float, float, NVGcolor icol, NVGcolor ocol) {
    NVGpaint paint = {}; paint.innerColor = icol; paint.outerColor = ocol; return paint;
}
inline NVGpaint nvgRadialGradient(NVGcontext*, float, float, float, float, NVGcolor icol, NVGcolor ocol) {
    NVGpaint paint = {}; paint.innerColor = icol; paint.outerColor = ocol; return paint;
}
inline NVGpaint nvgImagePattern(NVGcontext*, float, float, float, float, float, int, float) {
    NVGpaint paint = {}; return paint;
}

inline int nvgCreateImage(NVGcontext*, const char*, int) { return 0; }
inline int nvgCreateImageMem(NVGcontext*, int, unsigned char*, int) { return 0; }
inline int nvgCreateImageRGBA(NVGcontext*, int, int, int, const unsigned char*) { return 0; }
inline void nvgUpdateImage(NVGcontext*, int, const unsigned char*) {}
inline void nvgImageSize(NVGcontext*, int, int* w, int* h) { if (w) *w = 0; if (h) *h = 0; }
inline void nvgDeleteImage(NVGcontext*, int) {}

inline int nvgCreateFont(NVGcontext*, const char*, const char*) { return -1; }
inline int nvgCreateFontMem(NVGcontext*, const char*, unsigned char*, int, int) { return -1; }
inline int nvgFindFont(NVGcontext*, const char*) { return -1; }
inline int nvgAddFallbackFontId(NVGcontext*, int, int) { return 0; }
inline int nvgAddFallbackFont(NVGcontext*, const char*, const char*) { return 0; }
inline void nvgFontSize(NVGcontext*, float) {}
inline void nvgFontBlur(NVGcontext*, float) {}
inline void nvgTextLetterSpacing(NVGcontext*, float) {}
inline void nvgTextLineHeight(NVGcontext*, float) {}
inline void nvgTextAlign(NVGcontext*, int) {}
inline void nvgFontFaceId(NVGcontext*, int) {}
inline void nvgFontFace(NVGcontext*, const char*) {}
inline float nvgText(NVGcontext*, float x, float, const char*, const char*) { return x; }
inline void nvgTextBox(NVGcontext*, float, float, float, const char*, const char*) {}
inline float nvgTextBounds(NVGcontext*, float, float, const char*, const char*, float* bounds) {
    if (bounds) { bounds[0] = bounds[1] = bounds[2] = bounds[3] = 0.f; }
    return 0.f;
}
inline void nvgTextBoxBounds(NVGcontext*, float, float, float, const char*, const char*, float* bounds) {
    if (bounds) { bounds[0] = bounds[1] = bounds[2] = bounds[3] = 0.f; }
}
inline int nvgTextGlyphPositions(NVGcontext*, float, float, const char*, const char*, NVGglyphPosition*, int) { return 0; }
inline void nvgTextMetrics(NVGcontext*, float* ascender, float* descender, float* lineh) {
    if (ascender) *ascender = 0.f;
    if (descender) *descender = 0.f;
    if (lineh) *lineh = 0.f;
}
inline int nvgTextBreakLines(NVGcontext*, const char*, const char*, float, NVGtextRow*, int) { return 0; }

#endif
