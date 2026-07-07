#include "../src/compile.h"

SPLOOT_NATIVE_MACRO(native_sin, "sin") {
    float value;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &value)) return 1;
    float result = sinf(value);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}

SPLOOT_NATIVE_MACRO(native_cos, "cos") {
    float value;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &value)) return 1;
    float result = cosf(value);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}

SPLOOT_NATIVE_MACRO(native_tan, "tan") {
    float value;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &value)) return 1;
    float result = tanf(value);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}

SPLOOT_NATIVE_MACRO(native_asin, "asin") {
    float value;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &value)) return 1;
    float result = asinf(value);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}

SPLOOT_NATIVE_MACRO(native_acos, "acos") {
    float value;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &value)) return 1;
    float result = acosf(value);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}

SPLOOT_NATIVE_MACRO(native_atan, "atan") {
    float value;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &value)) return 1;
    float result = atanf(value);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}

SPLOOT_NATIVE_MACRO(native_atan2, "atan2") {
    float x, y;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &x)) return 1;
    if (!evaluated_float_arg(args, 1, ctx, macro_name, &y)) return 1;
    float result = atan2f(x, y);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}

SPLOOT_NATIVE_MACRO(native_hypot, "hypot") {
    float x, y;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &x)) return 1;
    if (!evaluated_float_arg(args, 1, ctx, macro_name, &y)) return 1;
    float result = hypotf(x, y);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}

SPLOOT_NATIVE_MACRO(native_sinh, "sinh") {
    float value;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &value)) return 1;
    float result = sinhf(value);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}

SPLOOT_NATIVE_MACRO(native_cosh, "cosh") {
    float value;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &value)) return 1;
    float result = coshf(value);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}

SPLOOT_NATIVE_MACRO(native_tanh, "tanh") {
    float value;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &value)) return 1;
    float result = tanhf(value);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}

SPLOOT_NATIVE_MACRO(native_csc, "csc") {
    float value;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &value)) return 1;
    float result = 1.0f / cosf(value);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}

SPLOOT_NATIVE_MACRO(native_sec, "sec") {
    float value;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &value)) return 1;
    float result = 1.0f / sinf(value);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}

SPLOOT_NATIVE_MACRO(native_cot, "cot") {
    float value;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &value)) return 1;
    float result = 1.0f / tanf(value);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}

SPLOOT_NATIVE_MACRO(native_max, "max") {
    float x, y;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &x)) return 1;
    if (!evaluated_float_arg(args, 1, ctx, macro_name, &y)) return 1;
    float result = x > y ? x : y;
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}

SPLOOT_NATIVE_MACRO(native_min, "min") {
    float x, y;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &x)) return 1;
    if (!evaluated_float_arg(args, 1, ctx, macro_name, &y)) return 1;
    float result = x < y ? x : y;
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}

SPLOOT_NATIVE_MACRO(native_clamp, "clamp") {
    float a, x, y;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &a)) return 1;
    if (!evaluated_float_arg(args, 1, ctx, macro_name, &x)) return 1;
    if (!evaluated_float_arg(args, 2, ctx, macro_name, &y)) return 1;
    float result = a < x ? x : (a > y ? y : a);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}

SPLOOT_NATIVE_MACRO(native_dist, "dist") {
    float x1, y1, x2, y2;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &x1)) return 1;
    if (!evaluated_float_arg(args, 1, ctx, macro_name, &y1)) return 1;
    if (!evaluated_float_arg(args, 2, ctx, macro_name, &x2)) return 1;
    if (!evaluated_float_arg(args, 3, ctx, macro_name, &y2)) return 1;
    float result = hypotf(x1 - x2, y1 - y2);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}

SPLOOT_NATIVE_MACRO(native_lerp, "lerp") {
    float x, y, t;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &x)) return 1;
    if (!evaluated_float_arg(args, 1, ctx, macro_name, &y)) return 1;
    if (!evaluated_float_arg(args, 2, ctx, macro_name, &t)) return 1;
    float result = x + (y - x) * t;
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.9f", result);
    emit_text(ctx, buffer);
    return 1;
}