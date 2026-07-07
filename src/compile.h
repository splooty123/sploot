#pragma once

#ifndef COMPILE
#define COMPILE

#include "pdf.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define TEXT_SIZE 12.0f
#define LINE_HEIGHT 20.0f
#define INDENT_WIDTH 36.0f
#define MAX_VARIABLES 1024
#define MAX_MACRO_ARGS 16
#define EXPANSION_DEPTH_LIMIT 64
#define MAX_WHILE_ITERATIONS 1000000
#define MAX_USER_MACROS 256
#define MAX_MACRO_PLACEHOLDERS 256

static float LEFT_MARGIN   = 50.0f;
static float TOP_MARGIN    = 750.0f;
static float BOTTOM_MARGIN = 50.0f;
static float RIGHT_MARGIN  = 550.0f;

typedef struct {
    float x_offset;
    float y_offset;
    float text_size;
} Compile_State;

typedef enum {
    NUMBER,
    STR
} Type_Id;

typedef struct {
    Type_Id type_id;
    int lifespan;
    float num_val;
    char* str_val;
    char name[64];
} User_Var;

typedef struct {
    char* data;
    size_t length;
    size_t capacity;
} String_Builder;

typedef struct {
    char* values[MAX_MACRO_ARGS];
    int count;
    const char* after;
} Macro_Args;

typedef struct {
    Compile_State* state;
    String_Builder* capture;
    int depth;
} Eval_Context;

// A user-defined macro created via ~macro(name)(param1)(param2)(body), in the
// spirit of LaTeX's \newcommand. `template` is the body with every reference
// to one of the declared parameters (e.g. @a) rewritten to "%s", and any
// literal '%' in the original body escaped to "%%" so it survives the
// substitution pass untouched. `arg_order` records, for each "%s" in
// left-to-right order, which parameter index (0-based, in declaration order)
// should be substituted there -- a parameter may appear zero, one, or many
// times in the body, and not necessarily in declaration order (e.g.
// ~macro(sub)(a)(b)(~expr(@b - @a)) needs arg_order = {1, 0}).
typedef struct {
    char name[64];
    int param_count;
    char* template;
    int arg_order[MAX_MACRO_PLACEHOLDERS];
    int placeholder_count;
} User_Macro;

static bool verbose;
static unsigned long verbose_step = 0;
static int declared_vars = 0;
static User_Var variables[MAX_VARIABLES];
static int declared_macros = 0;
static User_Macro user_macros[MAX_USER_MACROS];

// --- Hashing infrastructure --------------------------------------------------
//
// find_variable/find_user_macro/apply_macro's builtin dispatch all used to be
// O(n) linear scans (n = declared vars/macros, or the ~38-way strcmp chain
// for builtins). That's fine for a handful of lookups, but sploot programs
// that loop (~repeat, ~while, recursive user macros) can perform tens of
// thousands of these lookups, and user-defined macros previously had to fail
// every single builtin strcmp before even starting their own linear scan.
// Everything below replaces those scans with open-addressed hash tables so
// each lookup is O(1) amortized instead of O(n).

#define HASH_EMPTY     (-1)
#define HASH_TOMBSTONE (-2)

static uint64_t fnv1a_hash(const char* s) {
    uint64_t h = 1469598103934665603ULL; // FNV-1a 64-bit offset basis
    while (*s) {
        h ^= (unsigned char)(*s++);
        h *= 1099511628211ULL; // FNV-1a 64-bit prime
    }
    return h;
}

// Variables: up to MAX_VARIABLES (1024) entries, added via ~var and removed
// via ~free (which does a swap-with-last, so removal must also repoint the
// swapped-in entry's hash slot). Tombstones are needed here since deletion is
// a real, supported operation.
//
// Clearing uses a generation stamp per slot instead of physically rewriting
// every slot: "clear" just increments a counter, and a slot only counts as
// live if its stamp matches the current generation. That makes register_
// margin_vars() O(1) regardless of table size, so calling compile()/
// dump_expansion() many times (or even once, on a tiny document) never pays
// an O(table size) tax just to reset state.
#define VAR_HASH_SIZE 2048
#define VAR_HASH_MASK (VAR_HASH_SIZE - 1)
static int var_hash[VAR_HASH_SIZE];
static unsigned var_hash_stamp[VAR_HASH_SIZE];
static unsigned var_hash_generation = 1; // 0 is reserved to mean "never written"

static bool var_hash_slot_live(uint64_t i) {
    return var_hash_stamp[i] == var_hash_generation;
}

static void var_hash_clear(void) {
    var_hash_generation++;
    if (var_hash_generation == 0) { // wrapped after ~4 billion clears; reset for real
        memset(var_hash_stamp, 0, sizeof(var_hash_stamp));
        var_hash_generation = 1;
    }
}

static void var_hash_insert(const char* name, int var_idx) {
    uint64_t h = fnv1a_hash(name) & VAR_HASH_MASK;
    while (var_hash_slot_live(h) && var_hash[h] != HASH_TOMBSTONE) {
        h = (h + 1) & VAR_HASH_MASK;
    }
    var_hash[h] = var_idx;
    var_hash_stamp[h] = var_hash_generation;
}

static void var_hash_remove(const char* name) {
    uint64_t h = fnv1a_hash(name) & VAR_HASH_MASK;
    while (var_hash_slot_live(h)) {
        if (var_hash[h] != HASH_TOMBSTONE && strcmp(variables[var_hash[h]].name, name) == 0) {
            var_hash[h] = HASH_TOMBSTONE;
            return;
        }
        h = (h + 1) & VAR_HASH_MASK;
    }
}

// User macros: up to MAX_USER_MACROS (256), added via ~macro or redefined in
// place (same index, so no removal/tombstones needed here). Same generation-
// stamp trick for O(1) clearing.
#define MACRO_HASH_SIZE 512
#define MACRO_HASH_MASK (MACRO_HASH_SIZE - 1)
static int user_macro_hash[MACRO_HASH_SIZE];
static unsigned user_macro_hash_stamp[MACRO_HASH_SIZE];
static unsigned user_macro_hash_generation = 1;

static bool user_macro_hash_slot_live(uint64_t i) {
    return user_macro_hash_stamp[i] == user_macro_hash_generation;
}

static void user_macro_hash_clear(void) {
    user_macro_hash_generation++;
    if (user_macro_hash_generation == 0) {
        memset(user_macro_hash_stamp, 0, sizeof(user_macro_hash_stamp));
        user_macro_hash_generation = 1;
    }
}

static void user_macro_hash_insert(const char* name, int macro_idx) {
    uint64_t h = fnv1a_hash(name) & MACRO_HASH_MASK;
    while (user_macro_hash_slot_live(h)) {
        if (strcmp(user_macros[user_macro_hash[h]].name, name) == 0) {
            user_macro_hash[h] = macro_idx; // redefinition; same slot in practice
            return;
        }
        h = (h + 1) & MACRO_HASH_MASK;
    }
    user_macro_hash[h] = macro_idx;
    user_macro_hash_stamp[h] = user_macro_hash_generation;
}

// Builtin macros: a fixed, known-at-compile-time name list. The hash table
// only needs to store which Builtin_Macro_Id a bucket holds; the canonical
// name for collision confirmation lives in builtin_names[].
typedef enum {
    BM_COMMENT, BM_INCLUDE, BM_WIDTH, BM_CODE, BM_PRINT, BM_READ, BM_ASCII, BM_EXPR,
    BM_BRAINFUCK, BM_TILDE, BM_AT, BM_SET, BM_FREE, BM_VAR, BM_MACRO, BM_REPEAT, BM_WHILE,
    BM_LOREM, BM_TITLE, BM_CENTER, BM_XSHIFT, BM_YSHIFT, BM_XSET, BM_YSET, BM_SIZE,
    BM_NEWPAGE, BM_PAGE, BM_PAGEBREAK, BM_CLEARPAGE, BM_NL, BM_BR, BM_NEWLINE,
    BM_INDENT, BM_TAB, BM_PAR, BM_SPACE, BM_NBSP, BM_IMAGE,
    BM_COUNT
} Builtin_Macro_Id;

static const char* builtin_names[BM_COUNT] = {
    [BM_COMMENT] = "comment", [BM_INCLUDE] = "include", [BM_WIDTH] = "width", [BM_CODE] = "code",
    [BM_PRINT] = "print", [BM_READ] = "read", [BM_ASCII] = "ascii", [BM_EXPR] = "expr",
    [BM_BRAINFUCK] = "brainfuck", [BM_TILDE] = "tilde", [BM_AT] = "at", [BM_SET] = "set",
    [BM_FREE] = "free", [BM_VAR] = "var", [BM_MACRO] = "macro", [BM_REPEAT] = "repeat",
    [BM_WHILE] = "while", [BM_LOREM] = "lorem", [BM_TITLE] = "title", [BM_CENTER] = "center",
    [BM_XSHIFT] = "xshift", [BM_YSHIFT] = "yshift", [BM_XSET] = "xset", [BM_YSET] = "yset",
    [BM_SIZE] = "size", [BM_NEWPAGE] = "newpage", [BM_PAGE] = "page", [BM_PAGEBREAK] = "pagebreak",
    [BM_CLEARPAGE] = "clearpage", [BM_NL] = "nl", [BM_BR] = "br", [BM_NEWLINE] = "newline",
    [BM_INDENT] = "indent", [BM_TAB] = "tab", [BM_PAR] = "par", [BM_SPACE] = "space",
    [BM_NBSP] = "nbsp", [BM_IMAGE] = "image",
};

#define BUILTIN_HASH_SIZE 128
#define BUILTIN_HASH_MASK (BUILTIN_HASH_SIZE - 1)
static int builtin_hash[BUILTIN_HASH_SIZE];
static bool builtin_hash_ready = false;

static void builtin_hash_insert(const char* name, int id) {
    uint64_t h = fnv1a_hash(name) & BUILTIN_HASH_MASK;
    while (builtin_hash[h] != HASH_EMPTY) h = (h + 1) & BUILTIN_HASH_MASK;
    builtin_hash[h] = id;
}

static void ensure_builtin_dispatch_ready(void) {
    if (builtin_hash_ready) return;
    for (int i = 0; i < BUILTIN_HASH_SIZE; i++) builtin_hash[i] = HASH_EMPTY;
    for (int id = 0; id < BM_COUNT; id++) {
        builtin_hash_insert(builtin_names[id], id);
    }
    builtin_hash_ready = true;
}

// Returns the Builtin_Macro_Id for `name`, or -1 if it isn't a builtin.
static int lookup_builtin_id(const char* name) {
    ensure_builtin_dispatch_ready(); // safe to call from anywhere; no-op after the first time
    uint64_t h = fnv1a_hash(name) & BUILTIN_HASH_MASK;
    while (builtin_hash[h] != HASH_EMPTY) {
        if (strcmp(builtin_names[builtin_hash[h]], name) == 0) return builtin_hash[h];
        h = (h + 1) & BUILTIN_HASH_MASK;
    }
    return -1;
}

// --- Native macro registry ---------------------------------------------------
//
// A ~macro(...)-defined macro is always interpreted: every call re-walks its
// stored printf-style template and re-runs the sploot evaluator on the
// substituted body. Fine for glue code, but a "library" macro that gets
// called thousands of times per document (fast trig, a hot inner-loop
// primitive, a bulk numeric routine) pays that interpretation cost on every
// single call. sploot_register_native_macro() lets plain C register a real
// function under a macro name instead: ~name(...) jumps straight to native
// code, using the same helpers (evaluated_arg, evaluated_float_arg, emit_text,
// etc.) a builtin would use, with none of the template-replay overhead.
//
// Precedence is builtin > native > user-defined ~macro. Native registrations
// are meant to behave like fixed library entries (the same tier as a
// builtin): they're always available regardless of what a script defines,
// and they persist across multiple compile()/dump_expansion() calls in the
// same process (unlike ~var/~macro state, which is per-document and gets
// reset by register_margin_vars()). A script's own ~macro(name)... with a
// colliding name is accepted without error but is simply never reached --
// exactly like a script can't shadow the real ~set by defining its own.
//
// Because every helper in this file has internal (static) linkage, native
// macros must be defined in the same translation unit that #includes this
// header (i.e. add them to your main .c file, below the #include, or in
// another file you #include from there) -- not in a separately compiled .c.
typedef int (*Native_Macro_Fn)(const Macro_Args* args, Eval_Context* ctx, const char* macro_name);

typedef struct {
    char name[64];
    Native_Macro_Fn fn;
} Native_Macro;

#define MAX_NATIVE_MACROS 256
#define NATIVE_MACRO_HASH_SIZE 512
#define NATIVE_MACRO_HASH_MASK (NATIVE_MACRO_HASH_SIZE - 1)

static Native_Macro native_macros[MAX_NATIVE_MACROS];
static int declared_native_macros = 0;
static int native_macro_hash[NATIVE_MACRO_HASH_SIZE];
static bool native_macro_hash_ready = false;

static void ensure_native_macro_hash_ready(void) {
    if (native_macro_hash_ready) return;
    for (int i = 0; i < NATIVE_MACRO_HASH_SIZE; i++) native_macro_hash[i] = HASH_EMPTY;
    native_macro_hash_ready = true;
}

static int find_native_macro(const char* name) {
    if (!native_macro_hash_ready) return -1;
    uint64_t h = fnv1a_hash(name) & NATIVE_MACRO_HASH_MASK;
    while (native_macro_hash[h] != HASH_EMPTY) {
        if (strcmp(native_macros[native_macro_hash[h]].name, name) == 0) return native_macro_hash[h];
        h = (h + 1) & NATIVE_MACRO_HASH_MASK;
    }
    return -1;
}

// Public API: register a native macro. Call this once (e.g. at the top of
// main(), or automatically via SPLOOT_NATIVE_MACRO below) before compiling
// any document that uses it. Safe to call again for the same name to
// replace its implementation.
static void sploot_register_native_macro(const char* name, Native_Macro_Fn fn) {
    ensure_native_macro_hash_ready();
    ensure_builtin_dispatch_ready(); // so the builtin-collision check below is valid this early

    if (lookup_builtin_id(name) >= 0) {
        fprintf(stderr,
                "sploot: native macro \"%s\" collides with a builtin of the same name; ignoring registration\n",
                name);
        return;
    }

    int existing = find_native_macro(name);
    if (existing >= 0) {
        native_macros[existing].fn = fn;
        return;
    }

    if (declared_native_macros >= MAX_NATIVE_MACROS) {
        fprintf(stderr, "sploot: too many native macros registered; limit is %d\n", MAX_NATIVE_MACROS);
        return;
    }

    int idx = declared_native_macros++;
    strncpy(native_macros[idx].name, name, sizeof(native_macros[idx].name) - 1);
    native_macros[idx].name[sizeof(native_macros[idx].name) - 1] = '\0';
    native_macros[idx].fn = fn;

    uint64_t h = fnv1a_hash(name) & NATIVE_MACRO_HASH_MASK;
    while (native_macro_hash[h] != HASH_EMPTY) h = (h + 1) & NATIVE_MACRO_HASH_MASK;
    native_macro_hash[h] = idx;
}

// Convenience for defining + auto-registering a native macro in one line:
//
//   SPLOOT_NATIVE_MACRO(native_fastpow, "fastpow") {
//       float base, exponent;
//       if (!evaluated_float_arg(args, 0, ctx, macro_name, &base))     return 1;
//       if (!evaluated_float_arg(args, 1, ctx, macro_name, &exponent)) return 1;
//       char buf[64];
//       snprintf(buf, sizeof(buf), "%.9g", powf(base, exponent));
//       emit_text(ctx, buf);
//       return 1;
//   }
//
// This relies on GCC/Clang's constructor attribute to self-register before
// main() runs, so ~fastpow(...) works from the very first document you
// compile with no explicit setup call. On a compiler without constructor
// support, skip this macro and call sploot_register_native_macro() by hand
// near the top of main() instead.
#if defined(__GNUC__) || defined(__clang__)
#define SPLOOT_NATIVE_MACRO(fn_name, sploot_name)                                    \
    static int fn_name(const Macro_Args* args, Eval_Context* ctx,                    \
                        const char* macro_name);                                     \
    __attribute__((constructor)) static void fn_name##_sploot_register(void) {        \
        sploot_register_native_macro((sploot_name), fn_name);                        \
    }                                                                                 \
    static int fn_name(const Macro_Args* args, Eval_Context* ctx, const char* macro_name)
#endif

// -1 means unlimited (normal compile behavior). When >= 0, evaluate_fragment
// stops expanding macros/variables once ctx->depth reaches this value and
// instead echoes them back verbatim. Only meaningful in dump_expansion().
static int max_expansion_depth = -1;

static float* margin_ptrs[4];

static int evaluate_fragment(const char* src, Eval_Context* ctx, bool expand_vars);
static char* expand_argument_text(const char* raw, Eval_Context* parent_ctx);
static int evaluate_argument_fragment(const char* raw, Eval_Context* ctx);
static void draw_wrapped_text_span(char* start, char* end, Compile_State* state);
static void compile_space(Compile_State* state);

static void verbose_log(const char* stage, const char* fmt, ...) {
    if (!verbose) return;

    printf("[sploot:%06lu:%s] ", ++verbose_step, stage);

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\n");
}

static void* sploot_malloc(size_t size) {
    void* ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "sploot: out of memory\n");
        exit(1);
    }
    return ptr;
}

static char* copy_span(const char* start, size_t length) {
    char* copy = (char*)sploot_malloc(length + 1);
    memcpy(copy, start, length);
    copy[length] = '\0';
    return copy;
}

static char* copy_cstr(const char* text) {
    return copy_span(text, strlen(text));
}

static const char* type_name(Type_Id type_id) {
    switch (type_id) {
        case NUMBER: return "number";
        case STR: return "str";
    }
    return "unknown";
}

static void sb_init(String_Builder* sb) {
    sb->capacity = 128;
    sb->length = 0;
    sb->data = (char*)sploot_malloc(sb->capacity);
    sb->data[0] = '\0';
}

static void sb_reserve(String_Builder* sb, size_t needed) {
    if (needed <= sb->capacity) return;

    while (sb->capacity < needed) {
        sb->capacity *= 2;
    }

    char* data = (char*)realloc(sb->data, sb->capacity);
    if (!data) {
        fprintf(stderr, "sploot: out of memory growing expansion buffer\n");
        exit(1);
    }
    sb->data = data;
}

static void sb_append_len(String_Builder* sb, const char* text, size_t length) {
    if (length == 0) return;
    sb_reserve(sb, sb->length + length + 1);
    memcpy(sb->data + sb->length, text, length);
    sb->length += length;
    sb->data[sb->length] = '\0';
}

static void sb_append_cstr(String_Builder* sb, const char* text) {
    sb_append_len(sb, text, strlen(text));
}

static char* sb_take(String_Builder* sb) {
    char* data = sb->data;
    sb->data = NULL;
    sb->length = 0;
    sb->capacity = 0;
    return data;
}

static char* trim_copy(const char* text) {
    const char* start = text;
    const char* end = text + strlen(text);

    while (*start && isspace((unsigned char)*start)) start++;
    while (end > start && isspace((unsigned char)end[-1])) end--;

    return copy_span(start, (size_t)(end - start));
}

static bool is_name_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_name_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static bool is_macro_start(const char* text) {
    return text && text[0] == '~' && is_name_start(text[1]);
}

static bool is_var_start(const char* text) {
    return text && text[0] == '@' && is_name_start(text[1]);
}

static const char* name_end(const char* text) {
    const char* cursor = text + 1;
    while (is_name_char(*cursor)) cursor++;
    return cursor;
}

static void register_margin_vars(void) {
    struct { const char* name; float* ptr; float init; } defs[] = {
        { "leftmargin",   &LEFT_MARGIN,   50.0f  },
        { "rightmargin",  &RIGHT_MARGIN,  550.0f },
        { "topmargin",    &TOP_MARGIN,    750.0f },
        { "bottommargin", &BOTTOM_MARGIN, 50.0f  },
    };

    verbose_step = 0;
    declared_vars = 0;
    declared_macros = 0;
    var_hash_clear();
    user_macro_hash_clear();
    ensure_builtin_dispatch_ready();

    for (int i = 0; i < 4; i++) {
        strncpy(variables[i].name, defs[i].name, sizeof(variables[i].name) - 1);
        variables[i].name[sizeof(variables[i].name) - 1] = '\0';
        variables[i].type_id = NUMBER;
        variables[i].num_val = defs[i].init;
        variables[i].str_val = NULL;
        variables[i].lifespan = -1;
        margin_ptrs[i] = defs[i].ptr;
        *defs[i].ptr = defs[i].init;
        declared_vars++;
        var_hash_insert(variables[i].name, i);

        verbose_log("vars", "predeclared @%s = %g; the margin clipboard is wearing a tiny visor",
                    defs[i].name, defs[i].init);
    }
}

static void sync_margin_vars(void) {
    for (int i = 0; i < 4; i++) {
        *margin_ptrs[i] = variables[i].num_val;
    }
    verbose_log("layout", "synced margins: left=%g right=%g top=%g bottom=%g; the page ruler salutes",
                LEFT_MARGIN, RIGHT_MARGIN, TOP_MARGIN, BOTTOM_MARGIN);
}

static int find_variable(const char* name) {
    uint64_t h = fnv1a_hash(name) & VAR_HASH_MASK;
    while (var_hash_slot_live(h)) {
        if (var_hash[h] != HASH_TOMBSTONE && strcmp(variables[var_hash[h]].name, name) == 0) {
            return var_hash[h];
        }
        h = (h + 1) & VAR_HASH_MASK;
    }
    return -1;
}

static char* variable_value_string(int var_idx) {
    char output[1024];

    if (variables[var_idx].type_id == NUMBER) {
        snprintf(output, sizeof(output), "%.9g", variables[var_idx].num_val);
    } else {
        snprintf(output, sizeof(output), "%s", variables[var_idx].str_val ? variables[var_idx].str_val : "");
    }

    return copy_cstr(output);
}

static void require_expansion_depth(int depth, const char* phase) {
    if (depth > EXPANSION_DEPTH_LIMIT) {
        fprintf(stderr, "sploot: recursive %s expansion exceeded %d levels\n",
                phase, EXPANSION_DEPTH_LIMIT);
        exit(1);
    }
}

static char* expand_variables_only(const char* text, int depth) {
    require_expansion_depth(depth, "variable");

    String_Builder out;
    sb_init(&out);

    const char* cursor = text;
    while (*cursor) {
        if (is_var_start(cursor)) {
            const char* end = name_end(cursor);
            char* var_name = copy_span(cursor + 1, (size_t)(end - cursor - 1));
            int var_idx = find_variable(var_name);

            if (var_idx < 0) {
                fprintf(stderr, "Variable \"%s\" has not been declared yet or has been freed\n",
                        var_name);
                free(var_name);
                free(out.data);
                exit(1);
            }

            char* raw_value = variable_value_string(var_idx);
            char* expanded_value = expand_variables_only(raw_value, depth + 1);
            verbose_log("vars", "expanded @%s -> \"%s\" (%s); definition confetti remains compile-time",
                        var_name, expanded_value, type_name(variables[var_idx].type_id));

            sb_append_cstr(&out, expanded_value);
            free(expanded_value);
            free(raw_value);
            free(var_name);
            cursor = end;
        } else {
            sb_append_len(&out, cursor, 1);
            cursor++;
        }
    }

    return sb_take(&out);
}

static void emit_text_len(Eval_Context* ctx, const char* text, size_t length) {
    if (length == 0) return;

    if (ctx->capture) {
        sb_append_len(ctx->capture, text, length);
        verbose_log("emit", "captured %zu byte(s) of text expansion; extremely serious invisible ink",
                    length);
        return;
    }

    char* copy = copy_span(text, length);
    draw_wrapped_text_span(copy, copy + length, ctx->state);
    verbose_log("emit", "drew \"%s\" at size %.2f; the glyph cart rolled by",
                copy, ctx->state->text_size);
    free(copy);
}

static void emit_text(Eval_Context* ctx, const char* text) {
    emit_text_len(ctx, text, strlen(text));
}

static void emit_space(Eval_Context* ctx) {
    if (ctx->capture) {
        sb_append_cstr(ctx->capture, " ");
        verbose_log("emit", "captured one space; it insists it is structural whitespace");
    } else {
        compile_space(ctx->state);
    }
}

static float compile_line_height(Compile_State* state) {
    float height = state->text_size * (LINE_HEIGHT / TEXT_SIZE);
    verbose_log("layout", "line height is %.2f for text size %.2f; math has entered wearing boots",
                height, state->text_size);
    return height;
}

static void compile_new_page(Compile_State* state) {
    verbose_log("layout", "new page requested at y offset %.2f; page stack goes clack",
                state->y_offset);
    pdf_new_page();
    state->x_offset = 0.0f;
    state->y_offset = 0.0f;
}

static void compile_ensure_line_fits(Compile_State* state) {
    if (TOP_MARGIN - state->y_offset < BOTTOM_MARGIN) {
        verbose_log("layout", "line fell past bottom margin %.2f; invoking page elevator",
                    BOTTOM_MARGIN);
        compile_new_page(state);
    }
}

static float text_span_width(char* start, char* end, float text_size) {
    char saved = *end;
    float width;

    *end = '\0';
    width = text_width(start, text_size);
    *end = saved;

    verbose_log("layout", "span width %.2f for size %.2f; tape measure is emotionally available",
                width, text_size);
    return width;
}

static void draw_text_span(float x, float y, char* start, char* end,
                           float text_size) {
    char saved = *end;

    *end = '\0';
    draw_text(x, y, text_size, start);
    verbose_log("draw", "text \"%s\" at (%.2f, %.2f), size %.2f",
                start, x, y, text_size);
    *end = saved;
}

static void draw_wrapped_text_span(char* start, char* end, Compile_State* state) {
    float width;

    if (start == end) {
        return;
    }

    width = text_span_width(start, end, state->text_size);

    if (LEFT_MARGIN + state->x_offset + width > RIGHT_MARGIN) {
        verbose_log("layout", "wrapping before span width %.2f; the line put on a fresh shirt",
                    width);
        state->x_offset = 0.0f;
        state->y_offset += compile_line_height(state);
    }

    compile_ensure_line_fits(state);
    draw_text_span(LEFT_MARGIN + state->x_offset,
                   TOP_MARGIN - state->y_offset,
                   start,
                   end,
                   state->text_size);
    state->x_offset += width;
}

static void compile_newline(Compile_State* state) {
    verbose_log("layout", "newline at x=%.2f y=%.2f; carriage returns with paperwork",
                state->x_offset, state->y_offset);
    state->x_offset = 0.0f;
    state->y_offset += compile_line_height(state);
    compile_ensure_line_fits(state);
}

static void compile_indent(Compile_State* state) {
    verbose_log("layout", "indent %.2f from x=%.2f; paragraph elbows deployed",
                INDENT_WIDTH, state->x_offset);
    if (LEFT_MARGIN + state->x_offset + INDENT_WIDTH > RIGHT_MARGIN) {
        compile_newline(state);
    }

    state->x_offset += INDENT_WIDTH;
}

static void compile_space(Compile_State* state) {
    float width = text_glyph_width(' ', state->text_size);
    verbose_log("layout", "space width %.2f at x=%.2f; one blank citizen reports for duty",
                width, state->x_offset);

    if (LEFT_MARGIN + state->x_offset + width > RIGHT_MARGIN) {
        compile_newline(state);
    }

    state->x_offset += width;
}

static bool is_integer(const char* start, const char* end) {
    const char* cursor = start;

    while (cursor < end && isspace((unsigned char)*cursor)) cursor++;
    if (cursor < end && (*cursor == '-' || *cursor == '+')) cursor++;

    const char* first_digit = cursor;
    while (cursor < end && isdigit((unsigned char)*cursor)) cursor++;
    if (cursor == first_digit) return false;

    while (cursor < end && isspace((unsigned char)*cursor)) cursor++;
    return cursor == end;
}

static bool parse_float_value(const char* text, float* value) {
    char* trimmed = trim_copy(text);
    char* end;

    if (trimmed[0] == '\0') {
        free(trimmed);
        return false;
    }

    *value = strtof(trimmed, &end);
    if (end == trimmed) {
        free(trimmed);
        return false;
    }

    while (isspace((unsigned char)*end)) end++;
    bool ok = *end == '\0';
    free(trimmed);
    return ok;
}

static void macro_args_init(Macro_Args* args, const char* after_name) {
    args->count = 0;
    args->after = after_name;
    for (int i = 0; i < MAX_MACRO_ARGS; i++) {
        args->values[i] = NULL;
    }
}

static void macro_args_free(Macro_Args* args) {
    for (int i = 0; i < args->count; i++) {
        free(args->values[i]);
        args->values[i] = NULL;
    }
    args->count = 0;
}

static const char* skip_argument_gap(const char* cursor) {
    while (*cursor && isspace((unsigned char)*cursor)) cursor++;
    return cursor;
}

static bool parse_macro_args(const char* after_name, Macro_Args* args) {
    const char* cursor = after_name;
    macro_args_init(args, after_name);

    while (1) {
        const char* open = skip_argument_gap(cursor);
        if (*open != '(') {
            args->after = cursor;
            return true;
        }

        if (args->count >= MAX_MACRO_ARGS) {
            fprintf(stderr, "sploot: macro has more than %d arguments\n", MAX_MACRO_ARGS);
            macro_args_free(args);
            return false;
        }

        const char* arg_start = open + 1;
        const char* p = arg_start;
        int depth = 1;
        bool escaped = false;
        char quote = '\0';

        while (*p && depth > 0) {
            char c = *p;

            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (quote) {
                if (c == quote) quote = '\0';
            } else if (c == '\'' || c == '"') {
                quote = c;
            } else if (c == '(') {
                depth++;
            } else if (c == ')') {
                depth--;
                if (depth == 0) break;
            }

            p++;
        }

        if (depth != 0) {
            fprintf(stderr, "sploot: unclosed macro argument; the parser is holding a lonely '('\n");
            macro_args_free(args);
            return false;
        }

        args->values[args->count++] = copy_span(arg_start, (size_t)(p - arg_start));
        verbose_log("args", "parsed raw argument %d: \"%s\"",
                    args->count, args->values[args->count - 1]);
        cursor = p + 1;
        args->after = cursor;
    }
}

static bool require_arg_count(const char* macro_name, const Macro_Args* args, int expected) {
    if (args->count == expected) return true;

    fprintf(stderr, "~%s: expected %d argument%s but got %d\n",
            macro_name,
            expected,
            expected == 1 ? "" : "s",
            args->count);
    verbose_log("args", "~%s argument count mismatch; clipboard has filed a grievance",
                macro_name);
    return false;
}

static char* evaluated_arg(const Macro_Args* args, int index, Eval_Context* ctx,
                           const char* macro_name) {
    if (index < 0 || index >= args->count) {
        fprintf(stderr, "~%s: missing argument %d\n", macro_name, index + 1);
        return NULL;
    }

    char* value = expand_argument_text(args->values[index], ctx);
    verbose_log("args", "~%s argument %d evaluated to \"%s\"",
                macro_name, index + 1, value);
    return value;
}

static bool evaluated_float_arg(const Macro_Args* args, int index, Eval_Context* ctx,
                                const char* macro_name, float* value) {
    char* arg = evaluated_arg(args, index, ctx, macro_name);
    bool ok;

    if (!arg) return false;
    ok = parse_float_value(arg, value);
    if (!ok) {
        fprintf(stderr, "~%s: argument %d must be a float, got \"%s\"\n",
                macro_name, index + 1, arg);
    }
    free(arg);
    return ok;
}

static bool evaluated_int_arg(const Macro_Args* args, int index, Eval_Context* ctx,
                              const char* macro_name, int* value) {
    char* arg = evaluated_arg(args, index, ctx, macro_name);
    bool ok;

    if (!arg) return false;
    ok = is_integer(arg, arg + strlen(arg));
    if (ok) {
        *value = atoi(arg);
    } else {
        fprintf(stderr, "~%s: argument %d must be an integer, got \"%s\"\n",
                macro_name, index + 1, arg);
    }
    free(arg);
    return ok;
}

static int find_user_macro(const char* name) {
    uint64_t h = fnv1a_hash(name) & MACRO_HASH_MASK;
    while (user_macro_hash_slot_live(h)) {
        if (strcmp(user_macros[user_macro_hash[h]].name, name) == 0) return user_macro_hash[h];
        h = (h + 1) & MACRO_HASH_MASK;
    }
    return -1;
}

// Rewrites a ~macro body into a printf-style template: every @name that
// matches one of the macro's own parameters becomes "%s" (recorded in
// arg_order, in the order encountered), any @name that doesn't match a
// parameter is left alone so it resolves as a normal global variable at
// invocation time, and any literal '%' already in the body is escaped to
// "%%" so it can't be mistaken for a substitution later. Returns NULL (and
// sets *out_count to -1) if the body uses more parameter references than
// MAX_MACRO_PLACEHOLDERS supports.
static char* build_macro_template(const char* raw_body, char** param_names, int param_count,
                                  int* arg_order, int* out_count) {
    String_Builder sb;
    sb_init(&sb);
    int count = 0;
    const char* cursor = raw_body;

    while (*cursor) {
        if (*cursor == '%') {
            sb_append_cstr(&sb, "%%");
            cursor++;
            continue;
        }

        if (is_var_start(cursor)) {
            const char* end = name_end(cursor);
            size_t len = (size_t)(end - cursor - 1);
            int matched = -1;

            for (int i = 0; i < param_count; i++) {
                if (strlen(param_names[i]) == len &&
                    strncmp(param_names[i], cursor + 1, len) == 0) {
                    matched = i;
                    break;
                }
            }

            if (matched >= 0) {
                if (count >= MAX_MACRO_PLACEHOLDERS) {
                    fprintf(stderr, "~macro: body references parameters more than %d times\n",
                            MAX_MACRO_PLACEHOLDERS);
                    free(sb.data);
                    *out_count = -1;
                    return NULL;
                }
                sb_append_cstr(&sb, "%s");
                arg_order[count++] = matched;
                cursor = end;
                continue;
            }

            sb_append_len(&sb, cursor, (size_t)(end - cursor));
            cursor = end;
            continue;
        }

        sb_append_len(&sb, cursor, 1);
        cursor++;
    }

    *out_count = count;
    return sb_take(&sb);
}

static void free_declared_macros(void) {
    for (int i = 0; i < declared_macros; i++) {
        free(user_macros[i].template);
        user_macros[i].template = NULL;
    }
}

static void set_variable_from_text(int var_idx, const char* value) {
    float num_value;

    if (parse_float_value(value, &num_value)) {
        variables[var_idx].type_id = NUMBER;
        variables[var_idx].num_val = num_value;
        variables[var_idx].str_val = NULL;
    } else {
        variables[var_idx].type_id = STR;
        variables[var_idx].num_val = 0.0f;
        variables[var_idx].str_val = copy_cstr(value);
    }
    variables[var_idx].lifespan = -1;
}

static bool update_variable_from_text(int var_idx, const char* value) {
    if (variables[var_idx].type_id == NUMBER) {
        float num_value;
        if (parse_float_value(value, &num_value)) {
            variables[var_idx].num_val = num_value;
            return true;
        }
        return false;
    }

    if (variables[var_idx].type_id == STR) {
        free(variables[var_idx].str_val);
        variables[var_idx].str_val = copy_cstr(value);
        return true;
    }

    return false;
}

static bool free_variable_by_name(const char* name) {
    int var_idx = find_variable(name);

    if (var_idx < 0) {
        fprintf(stderr, "No variable matching \"%s\" exists\n", name);
        return false;
    }

    if (var_idx < 4) {
        fprintf(stderr, "Cannot free built-in margin variable \"%s\"\n", name);
        return false;
    }

    int last = declared_vars - 1;
    if (variables[var_idx].type_id == STR) {
        free(variables[var_idx].str_val);
    }

    var_hash_remove(variables[var_idx].name);

    if (var_idx != last) {
        var_hash_remove(variables[last].name);
        variables[var_idx] = variables[last];
        var_hash_insert(variables[var_idx].name, var_idx);
    }

    declared_vars--;
    verbose_log("vars", "freed @%s; variable table now has %d entries",
                name, declared_vars);
    return true;
}

static void emit_centered_text(Eval_Context* ctx, const char* text, bool title) {
    float prev_size = ctx->state->text_size;

    if (title) {
        ctx->state->text_size = 24.0f;
    }

    float width = text_width(text, ctx->state->text_size);
    ctx->state->x_offset = (RIGHT_MARGIN - LEFT_MARGIN - width) / 2.0f;
    compile_ensure_line_fits(ctx->state);
    draw_text(LEFT_MARGIN + ctx->state->x_offset,
              TOP_MARGIN - ctx->state->y_offset,
              ctx->state->text_size,
              text);
    verbose_log("draw", "%s \"%s\" centered with width %.2f",
                title ? "title" : "text", text, width);

    if (title) {
        ctx->state->text_size = prev_size;
        compile_newline(ctx->state);
        ctx->state->x_offset = 0.0f;
    } else {
        ctx->state->x_offset += width;
    }
}

enum Expr_Type { EXPR_NUM, EXPR_STR };

typedef struct {
    enum Expr_Type type;
    float num;
    char str[512];
} Expr_Value;

// --- Tokenizer -------------------------------------------------------------
//
// Turns "3 + 4 * (2 - 1)" into a flat token stream. Numbers, parens, and the
// operator set below are recognized directly; anything else contiguous
// (e.g. text left behind by variable expansion, like "Ada") becomes a bare
// string token, mirroring the old RPN evaluator's "unknown token = string"
// behavior.

typedef enum { EXPR_TOK_NUM, EXPR_TOK_STR, EXPR_TOK_OP, EXPR_TOK_END } Expr_Tok_Type;

typedef struct {
    Expr_Tok_Type type;
    char text[256];
    float num;
} Expr_Token;

#define EXPR_MAX_TOKENS 256

static int tokenize_expr(const char* src, Expr_Token* toks, int max_toks) {
    int n = 0;
    const char* p = src;

    while (*p) {
        if (isspace((unsigned char)*p)) { p++; continue; }
        if (n >= max_toks - 1) { // leave room for the EXPR_TOK_END sentinel
            fprintf(stderr, "~expr: expression too long\n");
            return -1;
        }

        if ((p[0] == '&' && p[1] == '&') || (p[0] == '|' && p[1] == '|') ||
            (p[0] == '*' && p[1] == '*') || (p[0] == '!' && p[1] == '=') ||
            (p[0] == '>' && p[1] == '=') || (p[0] == '<' && p[1] == '=')) {
            toks[n].type = EXPR_TOK_OP;
            toks[n].text[0] = p[0];
            toks[n].text[1] = p[1];
            toks[n].text[2] = '\0';
            n++;
            p += 2;
            continue;
        }

        if (strchr("+-*/%=<>?:()", *p)) {
            toks[n].type = EXPR_TOK_OP;
            toks[n].text[0] = *p;
            toks[n].text[1] = '\0';
            n++;
            p++;
            continue;
        }

        if (isdigit((unsigned char)*p)) {
            const char* start = p;
            while (isdigit((unsigned char)*p)) p++;
            if (*p == '.' && isdigit((unsigned char)p[1])) {
                p++;
                while (isdigit((unsigned char)*p)) p++;
            }
            // Scientific notation, e.g. "6.2270208e+09" -- variables print
            // their values via "%.9g", which switches to this form for any
            // number needing more than 9 significant digits (common once
            // ~factorial or similar gets past single-digit inputs), so the
            // tokenizer has to accept it or every such value becomes
            // unparsable garbage the moment it's read back into ~expr.
            if ((*p == 'e' || *p == 'E') &&
                (isdigit((unsigned char)p[1]) ||
                 ((p[1] == '+' || p[1] == '-') && isdigit((unsigned char)p[2])))) {
                p++;
                if (*p == '+' || *p == '-') p++;
                while (isdigit((unsigned char)*p)) p++;
            }
            size_t len = (size_t)(p - start);
            if (len >= sizeof(toks[n].text)) len = sizeof(toks[n].text) - 1;
            memcpy(toks[n].text, start, len);
            toks[n].text[len] = '\0';
            toks[n].type = EXPR_TOK_NUM;
            toks[n].num = strtof(toks[n].text, NULL);
            n++;
            continue;
        }

        // A variable that has overflowed (e.g. a large ~factorial result)
        // prints as literal "inf"/"nan" text via the same strtof-compatible
        // round trip used everywhere else in this file. Without recognizing
        // that text here too, it would fall through to the bareword-string
        // branch below and every arithmetic op on it would wrongly report
        // "requires numeric operands" even though it's a perfectly good
        // (if extreme) numeric value.
        if (isalpha((unsigned char)*p)) {
            static const struct { const char* word; size_t len; } inf_nan_forms[] = {
                { "infinity", 8 }, { "inf", 3 }, { "nan", 3 },
            };
            bool matched = false;

            for (size_t f = 0; f < sizeof(inf_nan_forms) / sizeof(inf_nan_forms[0]); f++) {
                size_t wlen = inf_nan_forms[f].len;
                bool word_matches = true;
                for (size_t k = 0; k < wlen; k++) {
                    if (tolower((unsigned char)p[k]) != inf_nan_forms[f].word[k]) {
                        word_matches = false;
                        break;
                    }
                }
                if (!word_matches) continue;

                char after = p[wlen];
                bool at_boundary = after == '\0' || isspace((unsigned char)after) ||
                                   strchr("+-*/%=<>?:()\"'", after) != NULL;
                if (!at_boundary) continue;

                size_t len = wlen;
                if (len >= sizeof(toks[n].text)) len = sizeof(toks[n].text) - 1;
                memcpy(toks[n].text, p, len);
                toks[n].text[len] = '\0';
                toks[n].type = EXPR_TOK_NUM;
                toks[n].num = strtof(toks[n].text, NULL);
                n++;
                p += wlen;
                matched = true;
                break;
            }

            if (matched) continue;
        }

        // Quoted string literal. Quotes are optional -- a bare word like
        // `Ada` still tokenizes as a string below -- but wrapping in ' or "
        // lets a literal contain spaces, operators, or anything else that
        // would otherwise be chopped up by the tokenizer (e.g. "hi there",
        // 'a (b) c'). \" \' and \\ are recognized as escapes for including
        // the quote character or a literal backslash inside the literal.
        if (*p == '"' || *p == '\'') {
            char quote = *p;
            const char* start = p + 1;
            const char* q = start;
            bool escaped = false;

            while (*q && (escaped || *q != quote)) {
                if (escaped) escaped = false;
                else if (*q == '\\') escaped = true;
                q++;
            }

            if (*q != quote) {
                fprintf(stderr, "~expr: unterminated string literal\n");
                return -1;
            }

            size_t out_len = 0;
            size_t max_len = sizeof(toks[n].text) - 1;
            const char* r = start;
            while (r < q) {
                if (*r == '\\' && r + 1 < q && (r[1] == quote || r[1] == '\\')) {
                    r++;
                }
                if (out_len < max_len) {
                    toks[n].text[out_len++] = *r;
                }
                r++;
            }
            toks[n].text[out_len] = '\0';
            toks[n].type = EXPR_TOK_STR;
            n++;
            p = q + 1;
            continue;
        }

        {
            const char* start = p;
            while (*p && !isspace((unsigned char)*p) && !strchr("+-*/%=<>?:()\"'", *p)) {
                p++;
            }
            size_t len = (size_t)(p - start);
            if (len >= sizeof(toks[n].text)) len = sizeof(toks[n].text) - 1;
            memcpy(toks[n].text, start, len);
            toks[n].text[len] = '\0';
            toks[n].type = EXPR_TOK_STR;
            n++;
        }
    }

    return n;
}

// --- Pratt parser / evaluator -----------------------------------------------
//
// Standard precedence-climbing: parse_expr(min_bp) parses a prefix term via
// parse_nud(), then keeps folding in infix operators as long as their
// binding power is >= min_bp. Left-associative operators recurse with
// bp + 1 on the right side; right-associative ones (** and ?:) recurse
// with the same bp so they chain rightward.

#define EXPR_BP_TERNARY        1
#define EXPR_BP_OR             2
#define EXPR_BP_AND            3
#define EXPR_BP_EQUALITY       4
#define EXPR_BP_RELATIONAL     5
#define EXPR_BP_ADDITIVE       6
#define EXPR_BP_MULTIPLICATIVE 7
#define EXPR_BP_POWER          8

typedef struct {
    Expr_Token* toks;
    int count;
    int pos;
    bool error;
} Expr_Parser;

static Expr_Value expr_zero(void) { return (Expr_Value){ .type = EXPR_NUM, .num = 0.0f }; }
static Expr_Value expr_num(float v) { return (Expr_Value){ .type = EXPR_NUM, .num = v }; }

static Expr_Value expr_str(const char* s) {
    Expr_Value v = { .type = EXPR_STR };
    strncpy(v.str, s, sizeof(v.str) - 1);
    v.str[sizeof(v.str) - 1] = '\0';
    return v;
}

static Expr_Token* expr_peek(Expr_Parser* p) { return &p->toks[p->pos]; }

static Expr_Token* expr_advance(Expr_Parser* p) {
    Expr_Token* t = &p->toks[p->pos];
    if (p->pos < p->count) p->pos++;
    return t;
}

static bool expr_check_op(Expr_Parser* p, const char* op) {
    Expr_Token* t = expr_peek(p);
    return t->type == EXPR_TOK_OP && strcmp(t->text, op) == 0;
}

static int expr_infix_bp(const char* op) {
    if (!strcmp(op, "?"))  return EXPR_BP_TERNARY;
    if (!strcmp(op, "||")) return EXPR_BP_OR;
    if (!strcmp(op, "&&")) return EXPR_BP_AND;
    if (!strcmp(op, "=") || !strcmp(op, "!=")) return EXPR_BP_EQUALITY;
    if (!strcmp(op, "<") || !strcmp(op, ">") || !strcmp(op, "<=") || !strcmp(op, ">=")) return EXPR_BP_RELATIONAL;
    if (!strcmp(op, "+") || !strcmp(op, "-")) return EXPR_BP_ADDITIVE;
    if (!strcmp(op, "*") || !strcmp(op, "/") || !strcmp(op, "%")) return EXPR_BP_MULTIPLICATIVE;
    if (!strcmp(op, "**")) return EXPR_BP_POWER;
    return 0; // not an infix operator (also covers ')' ':' and end-of-input)
}

static bool expr_both_numeric(Expr_Parser* p, const Expr_Value* a, const Expr_Value* b, const char* op) {
    if (a->type != EXPR_NUM || b->type != EXPR_NUM) {
        fprintf(stderr, "~expr: '%s' requires numeric operands\n", op);
        p->error = true;
        return false;
    }
    return true;
}

static Expr_Value apply_binop(Expr_Parser* p, const char* op, Expr_Value a, Expr_Value b) {
    if (p->error) return expr_zero();

    if (!strcmp(op, "+") || !strcmp(op, "-") || !strcmp(op, "*") || !strcmp(op, "/") ||
        !strcmp(op, "%") || !strcmp(op, "&&") || !strcmp(op, "||") || !strcmp(op, "**")) {
        if (!expr_both_numeric(p, &a, &b, op)) return expr_zero();

        float r;
        if      (!strcmp(op, "+"))  r = a.num + b.num;
        else if (!strcmp(op, "-"))  r = a.num - b.num;
        else if (!strcmp(op, "*"))  r = a.num * b.num;
        else if (!strcmp(op, "/")) {
            if (b.num == 0.0f) { fprintf(stderr, "~expr: division by zero\n"); p->error = true; return expr_zero(); }
            r = a.num / b.num;
        } else if (!strcmp(op, "%")) {
            if (b.num == 0.0f) { fprintf(stderr, "~expr: modulo by zero\n"); p->error = true; return expr_zero(); }
            r = fmodf(a.num, b.num);
        } else if (!strcmp(op, "&&")) r = a.num && b.num;
        else if (!strcmp(op, "||"))   r = a.num || b.num;
        else                           r = powf(a.num, b.num); // **

        return expr_num(r);
    }

    if (!strcmp(op, "=") || !strcmp(op, "!=")) {
        bool eq;
        if (a.type == EXPR_STR && b.type == EXPR_STR) {
            eq = strcmp(a.str, b.str) == 0;
        } else if (a.type == EXPR_NUM && b.type == EXPR_NUM) {
            eq = a.num == b.num;
        } else {
            fprintf(stderr, "~expr: '%s' requires operands of the same type\n", op);
            p->error = true;
            return expr_zero();
        }
        return expr_num((float)(!strcmp(op, "=") ? eq : !eq));
    }

    // relational: < > <= >=
    if (!expr_both_numeric(p, &a, &b, op)) return expr_zero();
    float r;
    if      (!strcmp(op, "<"))  r = a.num <  b.num;
    else if (!strcmp(op, ">"))  r = a.num >  b.num;
    else if (!strcmp(op, "<=")) r = a.num <= b.num;
    else                        r = a.num >= b.num;
    return expr_num(r);
}

static Expr_Value parse_expr(Expr_Parser* p, int min_bp);

// nud: parses whatever can start an expression - a literal, a parenthesized
// group, or a prefix "-". Unary minus binds tighter than every binary op
// except **, so "-2 ** 2" is "-(2 ** 2)" == -4, matching the usual
// exponent-before-sign convention, while "-3 * 4" is "(-3) * 4" == -12.
static Expr_Value parse_nud(Expr_Parser* p) {
    if (p->error) return expr_zero();
    Expr_Token* t = expr_advance(p);

    if (t->type == EXPR_TOK_NUM) return expr_num(t->num);
    if (t->type == EXPR_TOK_STR) return expr_str(t->text);

    if (t->type == EXPR_TOK_OP && !strcmp(t->text, "(")) {
        Expr_Value inner = parse_expr(p, EXPR_BP_TERNARY);
        if (!expr_check_op(p, ")")) {
            fprintf(stderr, "~expr: expected ')'\n");
            p->error = true;
            return expr_zero();
        }
        expr_advance(p);
        return inner;
    }

    if (t->type == EXPR_TOK_OP && !strcmp(t->text, "-")) {
        Expr_Value operand = parse_expr(p, EXPR_BP_POWER);
        if (p->error) return expr_zero();
        if (operand.type != EXPR_NUM) {
            fprintf(stderr, "~expr: unary '-' requires a numeric operand\n");
            p->error = true;
            return expr_zero();
        }
        return expr_num(-operand.num);
    }

    fprintf(stderr, "~expr: unexpected token '%s' in expression\n", t->text);
    p->error = true;
    return expr_zero();
}

static Expr_Value parse_expr(Expr_Parser* p, int min_bp) {
    if (p->error) return expr_zero();
    Expr_Value left = parse_nud(p);

    while (!p->error) {
        Expr_Token* t = expr_peek(p);
        if (t->type != EXPR_TOK_OP) break;

        int bp = expr_infix_bp(t->text);
        if (bp == 0 || bp < min_bp) break;

        if (!strcmp(t->text, "?")) {
            expr_advance(p);
            Expr_Value then_val = parse_expr(p, EXPR_BP_TERNARY);
            if (p->error) return expr_zero();
            if (!expr_check_op(p, ":")) {
                fprintf(stderr, "~expr: expected ':' in ternary expression\n");
                p->error = true;
                return expr_zero();
            }
            expr_advance(p);
            Expr_Value else_val = parse_expr(p, EXPR_BP_TERNARY); // right-assoc chaining
            if (p->error) return expr_zero();

            if (left.type != EXPR_NUM) {
                fprintf(stderr, "~expr: '?' condition must be numeric\n");
                p->error = true;
                return expr_zero();
            }
            left = left.num ? then_val : else_val;
            continue;
        }

        char op[8];
        strncpy(op, t->text, sizeof(op) - 1);
        op[sizeof(op) - 1] = '\0';
        expr_advance(p);

        int rhs_min_bp = !strcmp(op, "**") ? bp : bp + 1; // ** is right-assoc, rest left-assoc
        Expr_Value right = parse_expr(p, rhs_min_bp);
        if (p->error) return expr_zero();

        left = apply_binop(p, op, left, right);
    }

    return left;
}

static int evaluate_expr(char* expression, Eval_Context* ctx) {
    Expr_Token tokens[EXPR_MAX_TOKENS];
    int n = tokenize_expr(expression, tokens, EXPR_MAX_TOKENS);
    if (n < 0) return 0;

    if (n == 0) {
        fprintf(stderr, "~expr: empty expression\n");
        return 0;
    }

    tokens[n].type = EXPR_TOK_END;
    tokens[n].text[0] = '\0';

    Expr_Parser parser = { .toks = tokens, .count = n, .pos = 0, .error = false };
    Expr_Value result = parse_expr(&parser, EXPR_BP_TERNARY);
    if (parser.error) return 0;

    if (parser.pos != parser.count) {
        fprintf(stderr, "~expr: unexpected trailing token '%s'\n", parser.toks[parser.pos].text);
        return 0;
    }

    if (result.type == EXPR_NUM) {
        char output[128];
        snprintf(output, sizeof(output), "%g", result.num);
        emit_text(ctx, output);
    } else {
        emit_text(ctx, result.str);
    }

    return 1;
}

static int apply_macro(const char* macro_name,
                       const Macro_Args* args,
                       Eval_Context* ctx,
                       char** after_macro) {
    bool capture = ctx->capture != NULL;

    *after_macro = (char*)args->after;
    verbose_log("macro", "enter ~%s with %d argument%s at depth %d%s",
                macro_name,
                args->count,
                args->count == 1 ? "" : "s",
                ctx->depth,
                capture ? " while capturing expansion text" : "");

    typedef struct {
        char instruction;
        int repeats;
    } Instruction_Span;

    int builtin_id = lookup_builtin_id(macro_name);

    switch (builtin_id) {

    case BM_COMMENT: {
        return 1;
    }

    case BM_INCLUDE: {
        if (!require_arg_count(macro_name, args, 1)) return 1;

        char* path = evaluated_arg(args, 0, ctx, macro_name);
        if (!path) return 1;

        char* trimmed_path = trim_copy(path);
        free(path);

        FILE* f = fopen(trimmed_path, "rb");
        if (!f) {
            fprintf(stderr, "~include: could not open file \"%s\"\n", trimmed_path);
            free(trimmed_path);
            return 1;
        }

        if (fseek(f, 0, SEEK_END) != 0) {
            fprintf(stderr, "~include: could not seek in \"%s\"\n", trimmed_path);
            fclose(f);
            free(trimmed_path);
            return 1;
        }
        long file_size = ftell(f);
        if (file_size < 0) {
            fprintf(stderr, "~include: could not determine size of \"%s\"\n", trimmed_path);
            fclose(f);
            free(trimmed_path);
            return 1;
        }
        rewind(f);

        char* file_contents = (char*)sploot_malloc((size_t)file_size + 1);
        size_t read_len = fread(file_contents, 1, (size_t)file_size, f);
        file_contents[read_len] = '\0';
        fclose(f);

        verbose_log("macro", "~include spliced in %zu byte(s) from \"%s\"",
                    read_len, trimmed_path);

        evaluate_argument_fragment(file_contents, ctx);

        free(file_contents);
        free(trimmed_path);
        return 1;
    }

    case BM_WIDTH: {
        if (!require_arg_count(macro_name, args, 1)) return 1;
        float width = text_width(evaluated_arg(args, 0, ctx, macro_name), ctx->state->text_size);
        char buf[32];
        snprintf(buf, sizeof(buf), "%.9f", width);
        emit_text(ctx, buf);
        return 1;
    }

    case BM_CODE: {
        if (!require_arg_count(macro_name, args, 1)) return 1;

        const char* raw = args->values[0];
        size_t len = strlen(raw);
        char* stripped = (char*)sploot_malloc(len + 1);
        size_t out_len = 0;
        for (size_t i = 0; i < len; i++) {
            if (!isspace((unsigned char)raw[i])) {
                stripped[out_len++] = raw[i];
            }
        }
        stripped[out_len] = '\0';

        verbose_log("macro", "~code stripped whitespace from body; \"%s\" -> \"%s\"; now executing",
                    raw, stripped);
        evaluate_argument_fragment(stripped, ctx);

        free(stripped);
        return 1;
    }

    case BM_PRINT: {
        if (!require_arg_count(macro_name, args, 1)) return 1;
        char* text = evaluated_arg(args, 0, ctx, macro_name);
        printf("%s\n", text);
        free(text);
        return 1;
    }

    case BM_READ: {
        if (!require_arg_count(macro_name, args, 1)) return 1;
        char* name = evaluated_arg(args, 0, ctx, macro_name);
        int idx = find_variable(name);
        if (idx < 0) {
            fprintf(stderr, "~read: variable not found\n");
            free(name);
            return 1;
        }
        if(variables[idx].type_id == STR) {
            emit_text(ctx, variables[idx].str_val);
        } else if(variables[idx].type_id == NUMBER) {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%.9g", variables[idx].num_val);
            emit_text(ctx, buffer);
        }
        free(name);
        return 1;
    }

    case BM_ASCII: {
        if (!require_arg_count(macro_name, args, 1)) return 1;
        char* expression = evaluated_arg(args, 0, ctx, macro_name);
        if (is_integer(expression, expression + strlen(expression))) {
            char ascii_code = atoi(expression);
            emit_text(ctx, (char[]){ (char)ascii_code, '\0' });
        } else {
            fprintf(stderr, "~ascii: argument must be an integer, got \"%s\"\n", expression);
        }
        free(expression);
        return 1;
    }

    case BM_EXPR: {
        if (!require_arg_count(macro_name, args, 1)) return 1;

        char* expression = evaluated_arg(args, 0, ctx, macro_name);
        if (!expression) return 1;
        evaluate_expr(expression, ctx);
        free(expression);
        return 1;
    }


    case BM_BRAINFUCK: {
        if (!require_arg_count(macro_name, args, 1)) return 1;

        char* code_text = evaluated_arg(args, 0, ctx, macro_name);
        if (!code_text) return 1;

        // Heap-allocated rather than stack-local: apply_macro is on the call
        // stack for every level of macro recursion (including user macros
        // that call themselves via ~macro), and ~2KB/level is fine where
        // ~1.3MB/level of stack-resident brainfuck buffers is not -- the
        // latter can blow the real C stack well before EXPANSION_DEPTH_LIMIT
        // ever gets a chance to catch runaway recursion cleanly.
        Instruction_Span* code_compressed = (Instruction_Span*)sploot_malloc(sizeof(Instruction_Span) * (1 << 16));
        int* jump_table_x = (int*)sploot_malloc(sizeof(int) * (1 << 16));
        int* jump_table_y = (int*)sploot_malloc(sizeof(int) * (1 << 16));
        unsigned char* tape = (unsigned char*)sploot_malloc(1 << 16);
        int* bf_stack = (int*)sploot_malloc(sizeof(int) * (1 << 16));
        memset(tape, 0, 1 << 16);
        uint16_t head = 0;
        int idx = 0;
        uint64_t steps = 0;

        int i = 0;
        int j = 0;
        while (code_text[i] != '\0' && j < (1 << 16) - 1) {
            char cur = code_text[i];
            if (!strchr("+-<>.,[]", cur)) {
                i++;
                continue;
            }

            int count = 1;
            if (cur != '[' && cur != ']') {
                while (code_text[i + count] == cur) count++;
            }
            code_compressed[j].instruction = cur;
            code_compressed[j].repeats = count;
            j++;
            i += count;
        }
        code_compressed[j].instruction = '\0';
        code_compressed[j].repeats = 0;

        int sp = 0;
        for (int k = 0; k < j; k++) {
            if (code_compressed[k].instruction == '[') {
                bf_stack[sp++] = k;
            } else if (code_compressed[k].instruction == ']') {
                if (sp == 0) {
                    fprintf(stderr, "~brainfuck: unmatched ']' at instruction %d\n", k);
                    free(code_compressed);
                    free(jump_table_x);
                    free(jump_table_y);
                    free(tape);
                    free(bf_stack);
                    free(code_text);
                    return 1;
                }
                int open = bf_stack[--sp];
                jump_table_x[open] = k;
                jump_table_y[k] = open;
            }
        }
        if (sp != 0) {
            fprintf(stderr, "~brainfuck: %d unmatched '[' in program\n", sp);
            free(code_compressed);
            free(jump_table_x);
            free(jump_table_y);
            free(tape);
            free(bf_stack);
            free(code_text);
            return 1;
        }

        verbose_log("macro", "~brainfuck compressed to %d instruction span%s; Skibit.",
                    j, j == 1 ? "" : "s");

        while (idx < j && steps < UINT64_MAX) {
            int repeats = code_compressed[idx].repeats;
            switch (code_compressed[idx].instruction) {
                case '+': tape[head] += (unsigned char)repeats; break;
                case '-': tape[head] -= (unsigned char)repeats; break;
                case '>': head += (uint16_t)repeats; break;
                case '<': head -= (uint16_t)repeats; break;
                case '.': {
                    char buf[2] = { (char)tape[head], '\0' };
                    for (int r = 0; r < repeats; r++) {
                        buf[0] = (char)tape[head];
                        emit_text(ctx, buf);
                        verbose_log("macro", "~brainfuck emitted byte %u as text",
                                    (unsigned int)tape[head]);
                    }
                    break;
                }
                case ',': tape[head] = 0; break;
                case '[': if (tape[head] == 0) idx = jump_table_x[idx]; break;
                case ']': if (tape[head] != 0) idx = jump_table_y[idx]; break;
            }
            idx++;
            steps++;
        }

        if (steps >= UINT64_MAX) {
            fprintf(stderr, "~brainfuck: step limit reached - possible infinite loop\n");
        }

        free(code_compressed);
        free(jump_table_x);
        free(jump_table_y);
        free(tape);
        free(bf_stack);
        free(code_text);
        return 1;
    }

    case BM_TILDE: {
        if (!require_arg_count(macro_name, args, 0)) return 1;
        emit_text(ctx, "~");
        return 1;
    }

    case BM_AT: {
        if (!require_arg_count(macro_name, args, 0)) return 1;
        emit_text(ctx, "@");
        return 1;
    }

    case BM_SET: {
        if (!require_arg_count(macro_name, args, 2)) return 1;

        char* name = evaluated_arg(args, 0, ctx, macro_name);
        char* value = evaluated_arg(args, 1, ctx, macro_name);
        if (!name || !value) {
            free(name);
            free(value);
            return 1;
        }

        char* trimmed_name = trim_copy(name);
        int var_idx = find_variable(trimmed_name);
        if (var_idx < 0) {
            fprintf(stderr, "No variable matching \"%s\" exists\n", trimmed_name);
            free(trimmed_name);
            free(name);
            free(value);
            return 1;
        }

        if (!update_variable_from_text(var_idx, value)) {
            fprintf(stderr, "Assignment type does not match declared type of variable in macro \"set\"\n");
            free(trimmed_name);
            free(name);
            free(value);
            return 1;
        }

        verbose_log("vars", "set @%s = \"%s\" (%s)",
                    trimmed_name, value, type_name(variables[var_idx].type_id));
        sync_margin_vars();
        free(trimmed_name);
        free(name);
        free(value);
        return 1;
    }

    case BM_FREE: {
        if (!require_arg_count(macro_name, args, 1)) return 1;

        char* name = evaluated_arg(args, 0, ctx, macro_name);
        if (!name) return 1;
        char* trimmed_name = trim_copy(name);
        free_variable_by_name(trimmed_name);
        free(trimmed_name);
        free(name);
        return 1;
    }

    case BM_VAR: {
        if (!require_arg_count(macro_name, args, 2)) return 1;

        if (declared_vars >= MAX_VARIABLES) {
            fprintf(stderr, "~var: too many variables; limit is %d\n", MAX_VARIABLES);
            return 1;
        }

        char* name = evaluated_arg(args, 0, ctx, macro_name);
        char* value = evaluated_arg(args, 1, ctx, macro_name);
        if (!name || !value) {
            free(name);
            free(value);
            return 1;
        }

        char* trimmed_name = trim_copy(name);
        if (!is_name_start(trimmed_name[0])) {
            fprintf(stderr, "~var: \"%s\" is not a valid variable name\n", trimmed_name);
            free(trimmed_name);
            free(name);
            free(value);
            return 1;
        }

        strncpy(variables[declared_vars].name, trimmed_name,
                sizeof(variables[declared_vars].name) - 1);
        variables[declared_vars].name[sizeof(variables[declared_vars].name) - 1] = '\0';
        set_variable_from_text(declared_vars, value);
        var_hash_insert(variables[declared_vars].name, declared_vars);
        verbose_log("vars", "declared @%s = \"%s\" as %s; it now lives in slot %d",
                    variables[declared_vars].name,
                    value,
                    type_name(variables[declared_vars].type_id),
                    declared_vars);
        declared_vars++;

        free(trimmed_name);
        free(name);
        free(value);
        return 1;
    }

    case BM_MACRO: {
        if (args->count < 2) {
            fprintf(stderr, "~macro: expected a name and a body (plus optional parameter names)\n");
            return 1;
        }

        int body_idx = args->count - 1;
        int param_count = args->count - 2;

        // Name and parameters are literal identifiers, like ~newcommand{\foo}
        // and its #1/#2 slots -- they are taken as raw text, not evaluated,
        // so defining a macro never runs macros/variable lookups as a side
        // effect. The body is likewise stored completely raw: it must NOT be
        // expanded now, since @a/@b are parameter placeholders that don't
        // exist as real variables until the macro is actually invoked.
        char* raw_name = trim_copy(args->values[0]);
        if (!is_name_start(raw_name[0])) {
            fprintf(stderr, "~macro: \"%s\" is not a valid macro name\n", raw_name);
            free(raw_name);
            return 1;
        }

        char* param_names[MAX_MACRO_ARGS];
        bool param_error = false;
        for (int i = 0; i < param_count; i++) {
            param_names[i] = trim_copy(args->values[1 + i]);
            if (!is_name_start(param_names[i][0])) {
                fprintf(stderr, "~macro: parameter \"%s\" is not a valid name\n", param_names[i]);
                param_error = true;
            }
        }
        if (param_error) {
            for (int i = 0; i < param_count; i++) free(param_names[i]);
            free(raw_name);
            return 1;
        }

        int macro_idx = find_user_macro(raw_name);
        if (macro_idx < 0) {
            if (declared_macros >= MAX_USER_MACROS) {
                fprintf(stderr, "~macro: too many macros; limit is %d\n", MAX_USER_MACROS);
                for (int i = 0; i < param_count; i++) free(param_names[i]);
                free(raw_name);
                return 1;
            }
            macro_idx = declared_macros++;
            user_macro_hash_insert(raw_name, macro_idx);
        } else {
            free(user_macros[macro_idx].template);
            verbose_log("macro", "redefining ~%s; the old paperwork gets shredded", raw_name);
        }

        strncpy(user_macros[macro_idx].name, raw_name, sizeof(user_macros[macro_idx].name) - 1);
        user_macros[macro_idx].name[sizeof(user_macros[macro_idx].name) - 1] = '\0';
        user_macros[macro_idx].param_count = param_count;

        int placeholder_count = 0;
        char* macro_template = build_macro_template(args->values[body_idx], param_names, param_count,
                                                     user_macros[macro_idx].arg_order, &placeholder_count);
        if (!macro_template) {
            for (int i = 0; i < param_count; i++) free(param_names[i]);
            free(raw_name);
            return 1;
        }

        user_macros[macro_idx].template = macro_template;
        user_macros[macro_idx].placeholder_count = placeholder_count;

        verbose_log("macro", "defined ~%s with %d parameter%s as template \"%s\"; newcommand energy achieved",
                    raw_name, param_count, param_count == 1 ? "" : "s", macro_template);

        for (int i = 0; i < param_count; i++) free(param_names[i]);
        free(raw_name);
        return 1;
    }

    case BM_REPEAT: {
        if (!require_arg_count(macro_name, args, 2)) return 1;

        float count;
        if (!evaluated_float_arg(args, 0, ctx, macro_name, &count)) return 1;
        if (count < 0) {
            fprintf(stderr, "~repeat: count must not be negative\n");
            return 1;
        }

        verbose_log("macro", "~repeat will evaluate its body %d time%s",
                    count, count == 1 ? "" : "s");
        for (int i = 0; i < (int)count; i++) {
            verbose_log("macro", "~repeat iteration %d/%d", i + 1, (int)count);
            evaluate_argument_fragment(args->values[1], ctx);
        }
        return 1;
    }

    case BM_WHILE: {
        if (!require_arg_count(macro_name, args, 2)) return 1;

        int iterations = 0;
        while (1) {
            // Re-evaluated fresh every pass (unlike ~repeat's count, which is
            // fixed up front) so ~set calls inside the body can actually
            // change the outcome.
            float condition;
            if (!evaluated_float_arg(args, 0, ctx, macro_name, &condition)) return 1;

            if (condition == 0.0f) {
                verbose_log("macro", "~while condition read 0 after %d iteration%s; the loop clocks out",
                            iterations, iterations == 1 ? "" : "s");
                break;
            }

            if (iterations >= MAX_WHILE_ITERATIONS) {
                fprintf(stderr, "~while: exceeded %d iterations without the condition reaching 0; assuming infinite loop\n",
                        MAX_WHILE_ITERATIONS);
                return 1;
            }

            iterations++;
            verbose_log("macro", "~while iteration %d (condition = %g); paperwork approved, proceeding",
                        iterations, condition);
            evaluate_argument_fragment(args->values[1], ctx);
        }
        return 1;
    }

    case BM_LOREM: {
        if (!require_arg_count(macro_name, args, 0)) return 1;

        const char* lorem =
            "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Nunc maximus commodo urna, id aliquet libero dignissim non. Sed laoreet non dui non pellentesque. Fusce eget nunc purus. Praesent volutpat facilisis nisi, sed pellentesque nunc mollis nec. Phasellus tempus interdum vestibulum. Phasellus sit amet libero faucibus arcu tempor volutpat. In dignissim diam velit, et porttitor eros ultricies sit amet. Donec felis arcu, ullamcorper sit amet est vel, commodo auctor sapien. Class aptent taciti sociosqu ad litora torquent per conubia nostra, per inceptos himenaeos. Donec consequat at dolor eget commodo. Ut sit amet porttitor risus. Donec urna ex, rutrum ac quam in, sagittis ultrices orci. Fusce sodales, magna eget varius convallis, tellus justo molestie elit, sed tempus nisi quam et justo. Sed semper vestibulum elit nec dignissim. Nullam id convallis turpis. "
            "Donec at tincidunt tortor, eget aliquam odio. Donec lacinia id velit et molestie. Nulla facilisi. Donec molestie orci nisi, eu porta nisi placerat eget. Nullam nec erat nec nunc pharetra facilisis. Quisque consectetur turpis non eleifend sagittis. Ut placerat dapibus sapien eu semper. Vivamus scelerisque tellus id vulputate viverra. Suspendisse malesuada mauris nunc, nec pharetra diam pellentesque ac. Donec venenatis ut felis vel sodales. Phasellus sed lectus et nisl mollis volutpat. Mauris placerat dictum felis, at pulvinar lacus condimentum at. "
            "Nullam neque ligula, vulputate sodales justo a, laoreet sollicitudin erat. Sed lacinia, neque nec commodo elementum, urna nisl convallis arcu, eu viverra libero sem nec nisi. Etiam vitae diam dui. Donec facilisis lobortis ornare. Pellentesque sollicitudin tristique nulla, a suscipit lacus malesuada vitae. Proin consequat pharetra justo, sed ultrices libero placerat vitae. Morbi vulputate nisl at purus egestas blandit. Ut sagittis felis et libero iaculis, nec sodales nunc suscipit. Pellentesque ut ipsum accumsan, malesuada arcu eget, mollis eros. Vestibulum nec elit felis. Ut tempor tellus nec ipsum venenatis fringilla a eu massa. Mauris luctus viverra turpis, non mattis turpis vehicula ac. Quisque sit amet massa eget orci interdum scelerisque et ut nulla. Mauris vel nulla id tellus pharetra auctor. Aenean porta diam pretium nisi egestas, sed semper sapien dapibus. Phasellus pulvinar sed turpis at facilisis. "
            "Aliquam eget sem fringilla, tempor turpis vel, ullamcorper lectus. Suspendisse nec velit ac metus imperdiet hendrerit. Maecenas fringilla, eros vel consectetur iaculis, mauris nulla lacinia odio, vitae iaculis augue lectus non nisl. Morbi aliquam sagittis diam in rhoncus. Suspendisse tincidunt nec quam ut convallis. Proin suscipit lectus quis massa cursus, eget varius ipsum iaculis. Phasellus vel turpis vitae odio dictum iaculis id suscipit mi. Integer in augue augue. Donec bibendum sem sem, eget consequat mi suscipit sed. "
            "Maecenas sagittis consectetur ultrices. Aliquam massa orci, scelerisque at imperdiet eu, euismod et dolor. Integer tempor hendrerit rutrum. Praesent ullamcorper cursus metus, vitae facilisis ipsum rutrum a. Proin efficitur sagittis mauris, ac egestas nisl tristique sed. Nullam elementum leo eget imperdiet convallis. Aliquam convallis justo augue, et finibus felis imperdiet nec. Fusce rhoncus consectetur leo, ac placerat est rhoncus sit amet. Praesent pharetra dui metus, vel convallis tellus tempus ut";

        if (capture) {
            emit_text(ctx, lorem);
            return 1;
        }

        char* lorem_copy = copy_cstr(lorem);
        char* word = strtok(lorem_copy, " ");
        bool first = true;

        while (word) {
            if (!first) compile_space(ctx->state);
            emit_text(ctx, word);
            first = false;
            word = strtok(NULL, " ");
        }

        free(lorem_copy);
        return 1;
    }

    case BM_TITLE:
    case BM_CENTER: {
        bool title = strcmp(macro_name, "title") == 0;
        if (!require_arg_count(macro_name, args, 1)) return 1;

        char* text = evaluated_arg(args, 0, ctx, macro_name);
        if (!text) return 1;

        if (capture) {
            emit_text(ctx, text);
        } else {
            emit_centered_text(ctx, text, title);
        }

        free(text);
        return 1;
    }

    case BM_XSHIFT:
    case BM_YSHIFT:
    case BM_XSET:
    case BM_YSET:
    case BM_SIZE: {
        if (!require_arg_count(macro_name, args, 1)) return 1;

        float value;
        if (!evaluated_float_arg(args, 0, ctx, macro_name, &value)) return 1;

        if (capture) {
            verbose_log("macro", "~%s is layout-only inside captured text",
                        macro_name);
            return 1;
        }

        if (strcmp(macro_name, "xshift") == 0) {
            ctx->state->x_offset += value;
            verbose_log("layout", "x shift by %.2f -> x=%.2f", value, ctx->state->x_offset);
        } else if (strcmp(macro_name, "yshift") == 0) {
            ctx->state->y_offset += value;
            verbose_log("layout", "y shift by %.2f -> y=%.2f", value, ctx->state->y_offset);
            compile_ensure_line_fits(ctx->state);
        } else if (strcmp(macro_name, "xset") == 0) {
            ctx->state->x_offset = value;
            verbose_log("layout", "x set to %.2f", ctx->state->x_offset);
        } else if (strcmp(macro_name, "yset") == 0) {
            ctx->state->y_offset = value;
            verbose_log("layout", "y set to %.2f", ctx->state->y_offset);
            compile_ensure_line_fits(ctx->state);
        } else {
            if (value <= 0.0f) {
                fprintf(stderr, "~size: size must be greater than zero\n");
                return 1;
            }
            ctx->state->text_size = value;
            verbose_log("layout", "text size is now %.2f",
                        value);
        }

        return 1;
    }

    case BM_NEWPAGE:
    case BM_PAGE:
    case BM_PAGEBREAK:
    case BM_CLEARPAGE: {
        if (!require_arg_count(macro_name, args, 0)) return 1;
        if (capture) {
            verbose_log("macro", "~%s is page-layout-only inside captured text; lovingly ignored",
                        macro_name);
        } else {
            compile_new_page(ctx->state);
        }
        return 1;
    }

    case BM_NL:
    case BM_BR:
    case BM_NEWLINE: {
        if (!require_arg_count(macro_name, args, 0)) return 1;
        if (capture) {
            sb_append_cstr(ctx->capture, "\n");
            verbose_log("emit", "captured newline");
        } else {
            compile_newline(ctx->state);
        }
        return 1;
    }

    case BM_INDENT:
    case BM_TAB: {
        if (!require_arg_count(macro_name, args, 0)) return 1;
        if (capture) {
            sb_append_cstr(ctx->capture, "    ");
            verbose_log("emit", "captured indent as four spaces");
        } else {
            compile_indent(ctx->state);
        }
        return 1;
    }

    case BM_PAR: {
        if (!require_arg_count(macro_name, args, 0)) return 1;
        if (capture) {
            sb_append_cstr(ctx->capture, "\n    ");
            verbose_log("emit", "captured paragraph break as newline plus spaces");
        } else {
            compile_newline(ctx->state);
            compile_indent(ctx->state);
        }
        return 1;
    }

    case BM_SPACE:
    case BM_NBSP: {
        if (!require_arg_count(macro_name, args, 0)) return 1;
        emit_space(ctx);
        return 1;
    }

    case BM_IMAGE: {
        if (!require_arg_count(macro_name, args, 3)) return 1;

        char* img_path = evaluated_arg(args, 0, ctx, macro_name);
        float draw_w;
        float draw_h;

        if (!img_path) return 1;
        if (!evaluated_float_arg(args, 1, ctx, macro_name, &draw_w) ||
            !evaluated_float_arg(args, 2, ctx, macro_name, &draw_h)) {
            free(img_path);
            return 1;
        }

        if (draw_w <= 0.0f || draw_h <= 0.0f) {
            fprintf(stderr, "~image: width and height must be greater than zero\n");
            free(img_path);
            return 1;
        }

        if (capture) {
            verbose_log("macro", "~image \"%s\" inside captured text has no text expansion; framed but invisible",
                        img_path);
            free(img_path);
            return 1;
        }

        int img_id = pdf_embed_image(img_path, NULL, NULL);
        if (!img_id) {
            fprintf(stderr, "~image: could not embed '%s'\n", img_path);
            free(img_path);
            return 1;
        }

        if (LEFT_MARGIN + ctx->state->x_offset + draw_w > RIGHT_MARGIN) {
            ctx->state->x_offset = 0.0f;
            ctx->state->y_offset += compile_line_height(ctx->state);
        }
        compile_ensure_line_fits(ctx->state);

        float pdf_x = LEFT_MARGIN + ctx->state->x_offset;
        float pdf_y = TOP_MARGIN - ctx->state->y_offset - draw_h;

        verbose_log("draw", "image '%s' at (%.2f, %.2f) size %.2fx%.2f",
                    img_path, pdf_x, pdf_y, draw_w, draw_h);

        pdf_draw_image(img_id, pdf_x, pdf_y, draw_w, draw_h);
        ctx->state->x_offset += draw_w;
        free(img_path);
        return 1;
    }

    default:
        break;
    } // switch (builtin_id)

    {
        int native_idx = find_native_macro(macro_name);
        if (native_idx >= 0) {
            return native_macros[native_idx].fn(args, ctx, macro_name);
        }
    }

    {
        int user_idx = find_user_macro(macro_name);
        if (user_idx >= 0) {
            User_Macro* m = &user_macros[user_idx];

            if (args->count != m->param_count) {
                fprintf(stderr, "~%s: expected %d argument%s but got %d\n",
                        macro_name, m->param_count, m->param_count == 1 ? "" : "s", args->count);
                return 1;
            }

            char* values[MAX_MACRO_ARGS];
            bool arg_error = false;
            for (int i = 0; i < m->param_count; i++) {
                values[i] = evaluated_arg(args, i, ctx, macro_name);
                if (!values[i]) arg_error = true;
            }
            if (arg_error) {
                for (int i = 0; i < m->param_count; i++) free(values[i]);
                return 1;
            }

            // Replay the stored template: "%%" -> literal '%', "%s" -> the
            // next argument value in arg_order, anything else copied as-is.
            String_Builder expanded;
            sb_init(&expanded);
            const char* t = m->template;
            int placeholder_idx = 0;

            while (*t) {
                if (t[0] == '%' && t[1] == '%') {
                    sb_append_len(&expanded, "%", 1);
                    t += 2;
                } else if (t[0] == '%' && t[1] == 's') {
                    int arg_i = m->arg_order[placeholder_idx++];
                    sb_append_cstr(&expanded, values[arg_i]);
                    t += 2;
                } else {
                    sb_append_len(&expanded, t, 1);
                    t++;
                }
            }

            char* body_instance = sb_take(&expanded);
            verbose_log("macro", "~%s(...) expanded via template -> \"%s\"; running the body now",
                        macro_name, body_instance);

            evaluate_argument_fragment(body_instance, ctx);

            free(body_instance);
            for (int i = 0; i < m->param_count; i++) free(values[i]);
            return 1;
        }
    }

    fprintf(stderr, "Pro tip: '%s' isn't a macro.\n", macro_name);
    verbose_log("macro", "~%s is unknown; leaving it as literal text", macro_name);
    return 0;
}

static int evaluate_argument_fragment(const char* raw, Eval_Context* ctx) {
    require_expansion_depth(ctx->depth + 1, "macro argument");

    Eval_Context nested = *ctx;
    nested.depth = ctx->depth + 1;
    verbose_log("eval", "body pass (sequential): \"%s\"", raw);
    int ok = evaluate_fragment(raw, &nested, true);
    return ok;
}

static char* expand_argument_text(const char* raw, Eval_Context* parent_ctx) {
    require_expansion_depth(parent_ctx->depth + 1, "macro argument");

    char* vars_expanded = expand_variables_only(raw, parent_ctx->depth + 1);
    String_Builder capture;
    sb_init(&capture);

    Compile_State state_copy = *parent_ctx->state;
    Eval_Context nested = {
        .state = &state_copy,
        .capture = &capture,
        .depth = parent_ctx->depth + 1,
    };

    verbose_log("eval", "argument variable pass: \"%s\" -> \"%s\"",
                raw, vars_expanded);
    evaluate_fragment(vars_expanded, &nested, false);
    free(vars_expanded);

    char* result = sb_take(&capture);
    verbose_log("eval", "argument macro pass produced \"%s\"", result);
    return result;
}

static int evaluate_fragment(const char* src, Eval_Context* ctx, bool expand_vars) {
    require_expansion_depth(ctx->depth, "fragment");

    const char* cursor = src;
    while (*cursor) {
        if (isspace((unsigned char)*cursor)) {
            while (isspace((unsigned char)*cursor)) cursor++;
            emit_space(ctx);
            continue;
        }

        if (is_macro_start(cursor)) {
            const char* macro_end = name_end(cursor);
            char* macro_name = copy_span(cursor + 1, (size_t)(macro_end - cursor - 1));
            Macro_Args args;
            char* after_macro = (char*)macro_end;

            if (!parse_macro_args(macro_end, &args)) {
                free(macro_name);
                return 0;
            }

            if (max_expansion_depth >= 0 && ctx->depth >= max_expansion_depth) {
                verbose_log("expand", "~%s left unexpanded; depth %d hit the -e limit of %d",
                            macro_name, ctx->depth, max_expansion_depth);
                emit_text_len(ctx, cursor, (size_t)(args.after - cursor));
                cursor = args.after;
                macro_args_free(&args);
                free(macro_name);
                continue;
            }

            int handled = apply_macro(macro_name, &args, ctx, &after_macro);
            if (handled) {
                cursor = after_macro;
                if (isspace((unsigned char)*cursor)) {
                    cursor++;
                }
            } else {
                emit_text_len(ctx, cursor, (size_t)(macro_end - cursor));
                cursor = macro_end;
            }

            macro_args_free(&args);
            free(macro_name);
            continue;
        }

        if (expand_vars && is_var_start(cursor)) {
            const char* var_token_end = name_end(cursor);

            if (max_expansion_depth >= 0 && ctx->depth >= max_expansion_depth) {
                verbose_log("expand", "variable at depth %d hit the -e limit of %d; left unexpanded",
                            ctx->depth, max_expansion_depth);
                emit_text_len(ctx, cursor, (size_t)(var_token_end - cursor));
                cursor = var_token_end;
                continue;
            }

            char* var_token = copy_span(cursor, (size_t)(var_token_end - cursor));
            char* vars_expanded = expand_variables_only(var_token, ctx->depth + 1);
            Eval_Context nested = *ctx;
            nested.depth = ctx->depth + 1;

            verbose_log("eval", "top-level variable pass: \"%s\" -> \"%s\"",
                        var_token, vars_expanded);
            evaluate_fragment(vars_expanded, &nested, false);

            free(vars_expanded);
            free(var_token);
            cursor = var_token_end;
            continue;
        }

        const char* start = cursor;
        while (*cursor &&
               !isspace((unsigned char)*cursor) &&
               !is_macro_start(cursor) &&
               !(expand_vars && is_var_start(cursor))) {
            cursor++;
        }

        emit_text_len(ctx, start, (size_t)(cursor - start));
    }

    return 1;
}

static void free_declared_string_vars(void) {
    for (int i = 4; i < declared_vars; i++) {
        if (variables[i].type_id == STR) {
            free(variables[i].str_val);
            variables[i].str_val = NULL;
        }
    }
}

static char* dump_expansion(const char* src, int max_depth) {
    max_expansion_depth = max_depth;
    register_margin_vars();

    if (!src) {
        src = "";
    }

    Compile_State state;
    state.x_offset = 0.0f;
    state.y_offset = 0.0f;
    state.text_size = TEXT_SIZE;

    String_Builder capture;
    sb_init(&capture);

    Eval_Context ctx = {
        .state = &state,
        .capture = &capture,
        .depth = 0,
    };

    verbose_log("expand", "dumping full expansion instead of compiling; the PDF backend stays home");
    evaluate_fragment(src, &ctx, true);
    verbose_log("expand", "expansion produced %zu byte(s)", capture.length);

    free_declared_string_vars();
    free_declared_macros();
    return sb_take(&capture);
}

static void compile(const char* src, const char* name) {
    Compile_State state;

    register_margin_vars();

    if (!src) {
        src = "";
    }

    state.x_offset = 0.0f;
    state.y_offset = 0.0f;
    state.text_size = TEXT_SIZE;

    verbose_log("compile", "starting document \"%s\"; Please don't give me a bad reviw on yelp please.",
                name);
    begin_document(name);

    Eval_Context ctx = {
        .state = &state,
        .capture = NULL,
        .depth = 0,
    };
    evaluate_fragment(src, &ctx, true);

    verbose_log("compile", "finished document \"%s\" at x=%.2f y=%.2f size=%.2f; Search up the bronze age meme.",
                name, state.x_offset, state.y_offset, state.text_size);
    end_document();
    free_declared_string_vars();
    free_declared_macros();
}

#endif