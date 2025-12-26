#include <stddef.h>
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Configuration
#define CC "gcc"
#define LINKER "gcc"
#define CFLAGS "-Wall -Werror -ggdb"
#define LIBS ""

#define SELF_CC CC

#define MAX_THREADS 8
#define PRINT_COMPILATION_COMMANDS 0
#define SHOW_UP_TO_DATE_MESSAGES 1
#define VERBOSE_BUILD_INFO 1

// Type definitions
typedef struct Cmd {
  const char **args;
  size_t count;
  size_t capacity;
} Cmd;

typedef struct Target Target;
struct Target {
  const char *output;
  Target **deps;
  Cmd *cmd;

  Target **parents;
  size_t parent_cnt;
  size_t parent_cap;
  atomic_int pending_deps;
  atomic_bool visited;

  int rebuild_needed;
  pthread_mutex_t lock;
};

typedef struct {
  Target **buf;
  size_t cap;
  size_t size;
  size_t head, tail;
  size_t total_cmd;

  pthread_mutex_t mutex;
  pthread_cond_t cond;
  atomic_bool done;
} Queue;

// Macros
#define panic(msg, ...)                                                        \
  do {                                                                         \
    fprintf(stderr, "Panic at %s:%d: \n>>> " msg "\n", __FILE__, __LINE__,     \
            ##__VA_ARGS__);                                                    \
    exit(1);                                                                   \
  } while (0)

#define DEPS(...)                                                              \
  (Target *[]) { __VA_ARGS__, NULL }

#define CMD(...)                                                               \
  &(Cmd){.args = (const char *[]){__VA_ARGS__, NULL},                          \
         .count =                                                              \
             (sizeof((const char *[]){__VA_ARGS__}) / sizeof(const char *)),   \
         .capacity = 0}

#define TARGET(name, out_file, deps_arr, cmd_ptr)                              \
  Target name = {.output = (out_file), .deps = (deps_arr), .cmd = (cmd_ptr)}

#define SOURCE(name, out_file)                                                 \
  Target name = {.output = (out_file), .deps = NULL, .cmd = NULL}

#define BINARY(name, output_file, sources, extra_deps, includes, linker,       \
               ldflags)                                                        \
  size_t name##_src_count = 0;                                                 \
  while (sources[name##_src_count])                                            \
    name##_src_count++;                                                        \
  Target **name##_objs = malloc(sizeof(Target *) * name##_src_count);          \
  for (size_t i = 0; i < name##_src_count; i++) {                              \
    name##_objs[i] = create_obj_target(sources[i], extra_deps, CC, includes);  \
  }                                                                            \
  Target *name = create_link_target(output_file, name##_objs,                  \
                                    name##_src_count, linker, ldflags)

// Forward declarations for functions used in main
void rebuild_self(int argc, char **argv);
void build_target(Target *root);

// Helper functions for BINARY macro
static inline char *obj_name_from_src(const char *src);
static inline Target *create_source_target(const char *src);
static inline Target *create_obj_target(const char *src, Target **extra_deps,
                                        const char *compiler,
                                        const char *includes);
static inline Target *create_link_target(const char *output, Target **objs,
                                         size_t obj_count, const char *linker,
                                         const char *ldflags);

// Global state
static pthread_mutex_t build_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int completed_cmds = 0;
static size_t total_nodes = 0;
