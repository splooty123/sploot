# Sploot

Sploot is a typesetting language but it's also a programming languge (???) which is based off of macro expansion.

It is named after my dog.

# Usage

```
./sploot -i input.sploot -o output.pdf
```

Flags:

* ``-i <path>``
    The input ``.sploot`` file. Required.
* ``-o <path>``
    The output ``.pdf`` file. Required unless ``--expand`` is given.
* ``--verbose``
    Logs what the compiler is doing as it expands macros and lays out text.
* ``--expand`` / ``-e [depth]``
    Instead of compiling to a PDF, dumps the fully macro-expanded text of the document. Useful for debugging what a macro actually expands to. If ``-o`` is also given, the expansion is written there instead of the PDF; otherwise it's printed to stdout. An optional integer ``depth`` stops expansion early at that recursion depth and prints unexpanded macros/variables past that point verbatim -- handy for seeing one level of expansion at a time instead of the fully resolved output.

# Text Based Macros

To write text in Sploot simply write text. Modifications to text are applied using "macros". A macro is started with the tilde character. For example, the macro ``~nl`` creates a newline character. Macros can recieve arguments through parenthesis, each argument gets their own parenthesis. For example, the macro ``~size(n)`` modifies all text after it to be size ``n``.

Macro arguments are evaluated recursively. Sploot expands variables first, then macros, and macro arguments do the same thing inside themselves, so constructs like ``~title(@name ~repeat(2)(!))`` work as compile-time expansions.

### Layout

* ``~size(x)``
    Sets the size of following text to ``x``.
* ``~xshift(x)`` / ``~yshift(y)``
    Shifts the following text by ``x``/``y`` units, relative to its current position.
* ``~xset(x)`` / ``~yset(y)``
    Moves following text to the absolute position ``x``/``y``, rather than shifting relative to where it currently is.
* ``~space`` (alias: ``~nbsp``)
    A single space.
* ``~indent`` (alias: ``~tab``)
    An indent.
* ``~nl`` (aliases: ``~br``, ``~newline``)
    A newline.
* ``~par``
    A newline followed by an indent -- i.e. a paragraph break.
* ``~newpage`` (aliases: ``~page``, ``~pagebreak``, ``~clearpage``)
    Starts following text on a new page.
* ``~center(t)``
    Centers the text ``t``.
* ``~title(t)``
    Centers the text ``t`` and also makes it big.
* ``~width(t)``
    Expands to the rendered width, in points, of text ``t`` at the current text size. Useful for laying things out by hand -- see ``~expr`` below.

### Content

* ``~image(path)(w)(h)``
    Draws the image from the specified path with width ``w`` and height ``h``.
* ``~lorem``
    Generic fake latin filler text.
* ``~repeat(n)(t)``
    Repeats the text ``t`` ``n`` times.
* ``~while(cond)(t)``
    Repeats the text ``t`` for as long as the ``~expr``-style condition ``cond`` evaluates to non-zero. ``cond`` is re-evaluated fresh before every iteration, so a ``~set`` inside ``t`` can end the loop.
* ``~ascii(n)``
    Emits the single raw character with ASCII code ``n``.
* ``~tilde`` / ``~at``
    Emit a literal ``~`` / ``@`` character. Needed since those two characters are otherwise special.
* ``~comment(...)``
    Consumes its argument and emits nothing. Use it to leave notes in a ``.sploot`` file without them ending up in the output.
* ``~code(...)``
    Strips all whitespace from its argument before evaluating it. Handy for writing a block of macro calls across multiple indented lines -- for layout, math, loops, variable bookkeeping, etc. -- without any of that surrounding whitespace leaking into the document as stray spaces.
* ``~print(t)``
    Prints ``t`` to the terminal at compile time (not into the PDF). A debugging tool, similar in spirit to ``--verbose`` but for your own values.
* ``~include(path)``
    Splices the contents of another file in at this point, exactly as if you'd pasted it in by hand, then evaluates it -- the same idea as C's ``#include``. Paths are relative to wherever you run ``sploot`` from.

### Compile-Time Math

* ``~expr(e)``
    Evaluates ``e`` as an arithmetic/logical expression and emits the result. Supports ``+ - * / % **`` (``**`` is exponentiation), comparisons ``< > <= >= = !=`` (note: equality is a single ``=``, not ``==``), boolean ``&& ||``, a C-style ternary ``cond ? a : b``, parentheses, and both number and string literals (quote a string with ``'`` or ``"`` if it contains spaces or symbols). Numbers and strings can be compared for equality, but arithmetic and relational operators require numeric operands.

    ```
    ~expr(2 + 2)                    -> 4
    ~expr((3 + 4) * 2)              -> 14
    ~expr(2 ** 10)                  -> 1024
    ~expr(@x > 5 ? "big" : "small") -> depends on @x
    ```

# Variables

Variables are declared with the macro ``~var(name)(val)`` where ``name`` is the name of the variable and ``val`` is the value you are setting it to. To use this variable append its name with the ``@`` character, doing this will substitute the variable use with its value. For example if I defined ``~var(One)(1)`` and then wrote ``@One`` it would output ``1``.

A variable is either a **Number** (which covers both integers and decimals -- ``1``, ``3.5``, and ``-2`` are all just Numbers) or a **String** (anything that doesn't parse as a number). Its type is inferred from the value the first time it's declared.

You can update an already-declared variable with the ``~set(name)(val)`` macro, which sets the variable ``name`` to the value ``val`` -- just make sure ``val``'s type (Number or String) matches the variable's existing type. ``~read(name)`` also reads out a variable's value, like ``@name`` does, except ``name`` itself is evaluated first -- which means you can look up a variable whose *name* is stored in another variable.

Lastly, there is a 1024 variable limit, to avoid reaching this limit and getting a mysterious segmentation fault use the macro ``~free(name)`` which frees the variable ``name``. Variable values are expanded at compile time, including recursive variable references and macros inside arguments.

# User-Defined Macros

You can define your own macros with ``~macro(name)(param1)(param2)...(body)``, in the spirit of LaTeX's ``\newcommand``. Any parameter written as ``@param`` inside the body gets substituted with the corresponding argument every time the macro is called; anything else in the body behaves exactly as if it had been written inline (including references to ordinary global variables, calls to other macros, etc.).

```
~macro(greet)(name)(Hello, @name! ~nl)
~greet(World)
```

Defining a macro with a name that's already taken just replaces the old definition. Macro bodies are stored raw and only expanded when called, so a macro can safely reference variables that don't exist yet at the point it's defined, as long as they exist by the time it's actually called.

# Writing Native Macros (for library authors)

``~macro``-defined macros are always interpreted -- every call re-runs the stored template through the sploot evaluator. That's fine for everyday glue, but a macro that's called thousands of times per document (fast trig, a hot inner-loop primitive, a bulk numeric routine) pays that interpretation cost every single time. For that, you can register a real C function as a macro instead:

```c
#include "compile.h"

SPLOOT_NATIVE_MACRO(native_fastpow, "fastpow") {
    float base, exponent;
    if (!evaluated_float_arg(args, 0, ctx, macro_name, &base)) return 1;
    if (!evaluated_float_arg(args, 1, ctx, macro_name, &exponent)) return 1;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.9g", powf(base, exponent));
    emit_text(ctx, buf);
    return 1;
}
```

``~fastpow(2)(10)`` now runs as native code with no interpretation overhead. ``SPLOOT_NATIVE_MACRO`` auto-registers the function before ``main()`` runs; native macro definitions need to live in the same translation unit that ``#include``s ``compile.h`` (see ``sploot.c`` / ``libs.h``), since everything in ``compile.h`` has internal linkage. A native macro is always available regardless of what a script defines, in the same way a builtin is -- a script can't shadow it by writing its own ``~macro`` of the same name.

# Compilation

Just put a ``.sploot`` file in the program and tell it your output pdf. Make sure that ``cmu.serif-roman.ttf`` is relative to the execution path.

ex. ``./sploot -i article.sploot -o article.pdf``

Use the ``--verbose`` flag to make it log information, or ``--expand`` to see what your document expands to without generating a PDF.

Building from source is a normal ``make``. Any file dropped in ``lib/`` (native macro libraries, written in C) is automatically pulled into the build via a generated ``src/libs.h`` -- see the ``Makefile``.

If you're unable to do this, email ``deansploot67@gmail.com`` with your .sploot source with the subject line "sploot" and I will compile for you and respond with the pdf.