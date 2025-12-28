# Build System (CB)

A **parallel, dependency-aware build system written in C**, inspired by `build.c` / `nob`, with explicit targets, automatic incremental rebuilds, file globbing, and self-rebuilding.

This is **not Make**, **not CMake**, and **not a DSL**.
You write C. The build graph is explicit. The behavior is predictable.

This project assumes that build logic is part of the program, not a separate domain that needs its own language.
---

## Design Goals

* Deterministic builds
* Explicit dependency graphs
* Parallel execution
* No external configuration language
* No magic dependency inference
* Easy to debug (it’s just C)

---

## Non-Goals (Important)

* No compiler-driven dependency scanning (`-MMD`)
* No automatic header parsing
* No implicit rules
* No platform abstraction layer beyond POSIX

If you want those, use Ninja + a generator. This tool is intentionally lower-level.

---

## Requirements

* GCC or Clang
* POSIX-compliant system (Linux, BSD, macOS)
* `pthread`

---

## Building the Build System

```sh
gcc build.c build.impl.c -o build -lpthread
```

The build system **rebuilds itself automatically** if its sources change.

---

## Running

```sh
./build
```

---

## Core Concepts

### Targets

A `Target` represents **one output file**.

* It may have:

  * a command (`Cmd`)
  * dependencies (`Target*`)
* Dependencies are built **before** the target
* Targets are built **once**, even in diamond graphs

---

### Source Targets

A source file is a target with **no build command**:

```c
Target *main_c = src("src/main.c");
```

This allows it to participate in dependency graphs.

---

### Command Objects

Commands are immutable argument vectors.

```c
Cmd *compile = CMD(CC, "-c", "main.c", "-o", "main.o", "-O2");
```

Commands can also be built incrementally:

```c
Cmd *cmd = cmd_new();
cmd_append(cmd, CC, "-c", "main.c", "-o", "main.o", NULL);
cmd_append(cmd, "-Wall", "-Wextra", "-O2", NULL);
```

You can clone and extend commands safely:

```c
Cmd *base = CMD(CC, "-c");
Cmd *flags = CMD("-O2", "-Wall");

Cmd *c1 = cmd_clone(base);
cmd_extend(c1, flags);
cmd_append(c1, "a.c", "-o", "a.o", NULL);
```

---

## Minimal Example

```c
#include "build.config.h"

int main(int argc, char **argv) {
    rebuild_self(argc, argv);

    ensure_dir("build");

    Target *main_o = target("build/main.o",
        CMD(CC, "-c", "src/main.c", "-o", "build/main.o", "-O2"),
        src("src/main.c"),
        NULL);

    Target *util_o = target("build/util.o",
        CMD(CC, "-c", "src/util.c", "-o", "build/util.o", "-O2"),
        src("src/util.c"),
        NULL);

    Target *app = target("build/app",
        CMD(CC, "build/main.o", "build/util.o", "-o", "build/app"),
        main_o, util_o, NULL);

    build_target(app);
}
```

---

## Ergonomic Macros (Recommended)

You are expected to add **project-local macros** for sanity.

### Object Rule Macro

```c
#define OBJ(out, srcfile, ...) \
    target(out, \
           CMD(CC, "-c", srcfile, "-o", out), \
           src(srcfile), \
           __VA_ARGS__, \
           NULL)
```

Usage:

```c
Target *main_o =
    OBJ("build/main.o",
        "src/main.c",
        src("include/main.h"));
```

---

### Grouping Headers

```c
#define COMMON_HDRS \
    src("include/common.h"), \
    src("include/config.h")

Target *util_o =
    OBJ("build/util.o",
        "src/util.c",
        COMMON_HDRS);
```

This avoids copy-paste dependency lists.

---

## Dependency Manipulation API

Dependencies are **first-class and mutable**.

```c
add_dep(obj, config_h);
add_deps(obj, h1, h2, h3, NULL);

remove_dep(obj, old_dep);
clear_deps(obj);

if (has_dep(obj, config_h)) { ... }

size_t n = dep_count(obj);
```

This is useful for:

* conditional features
* generated files
* platform-specific builds

---

## File Globbing

Globbing is explicit and returns **heap-allocated lists**.

```c
char **sources = glob_files("src", ".c");

for (int i = 0; sources[i]; i++) {
    printf("%s\n", sources[i]);
}

free_glob(sources);
```

No automatic target creation is done for you.

---

## Example: Compile All `.c` Files

```c
char **sources = glob_files("src", ".c");

int count = 0;
while (sources[count]) count++;

Target **objs = malloc(sizeof(Target*) * count);

for (int i = 0; i < count; i++) {
    char obj[256];
    snprintf(obj, sizeof(obj), "build/%d.o", i);

    objs[i] = target(strdup(obj),
        CMD(CC, "-c", sources[i], "-o", obj, "-O2"),
        src(sources[i]),
        NULL);
}
```

---

## Example: Generated Headers

```c
Target *config_h =
    target("build/config.h",
        CMD("sh", "-c", "scripts/gen_config.sh > build/config.h"),
        src("scripts/gen_config.sh"),
        NULL);

add_dep(main_o, config_h);
add_dep(util_o, config_h);
```

Generated files behave like normal targets.

---

## Parallelism

* Uses a thread pool
* Maximum threads configurable
* `MAX_THREADS = 0` auto-detects CPU count
* Dependency ordering is respected

---

## Diamond Dependencies

If multiple targets depend on the same target:

```text
A
├── B
│   └── D
└── C
    └── D
```

`D` is built **once**, correctly.

---

## Self-Rebuilding

```c
rebuild_self(argc, argv);
```

If the build system source changes:

* it recompiles itself
* re-execs automatically

No manual bootstrapping required.

---

## Configuration (`build.config.h`)

```c
#define CC "gcc"
#define LINKER "gcc"
#define SELF_CC CC
#define MAX_THREADS 0
```

For cross-compilation:

```c
#define CC "x86_64-w64-mingw32-gcc"
#define LINKER CC
#define SELF_CC "gcc"
```

---

## Debugging Philosophy

This system is designed to be debugged with:

* `printf`
* `gdb`
* reading the source

There is **no opaque runtime**, no hidden state machine, and no generator step.

---

## When This Tool Makes Sense

* Small–medium C/C++ projects
* Custom build logic
* Tooling experiments
* Systems programming projects
* Replacing ad-hoc Makefiles

## When It Does Not

* Huge multi-platform SDKs
* IDE-driven workflows
* Projects requiring automatic header scanning
* Teams expecting CMake semantics

---

## License

See `LICENSE`.
