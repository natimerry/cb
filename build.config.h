#ifndef BUILD_CONFIG_H
#define BUILD_CONFIG_H

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CC "gcc"
#define LINKER "gcc"
#define CFLAGS "-Wall -Werror -ggdb"
#define LIBS ""
#define SELF_CC CC

#define MAX_THREADS -1
#define PRINT_COMPILATION_COMMANDS 0
#define SHOW_UP_TO_DATE_MESSAGES 1
#define VERBOSE_BUILD_INFO 1

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
  _Atomic int pending_deps;
  _Atomic bool visited;

  int rebuild_needed;
  pthread_mutex_t lock; // pthread_mutex_t
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

#define panic(msg, ...)                                                        \
  do {                                                                         \
    fprintf(stderr, "Panic at %s:%d: \n>>> " msg "\n", __FILE__, __LINE__,     \
            ##__VA_ARGS__);                                                    \
    exit(1);                                                                   \
  } while (0)

#define CMD(...)                                                               \
  &(Cmd){.args = (const char *[]){__VA_ARGS__, NULL},                          \
         .count =                                                              \
             (sizeof((const char *[]){__VA_ARGS__}) / sizeof(const char *)),   \
         .capacity = 0};

void cmd_internal_append(Cmd *cmd, ...);
#define cmd_append(CMD, ...) cmd_internal_append(CMD, __VA_ARGS__, NULL)
void cmd_extend(Cmd *cmd1, const Cmd *cmd2);
Cmd *cmd_clone(const Cmd *src);
Cmd *cmd_new(void);
void cmd_print(const Cmd *cmd);
int cmd_run(Cmd *cmd, const char *task_out, int task_num, size_t total_cmd);

Target *src(const char *path);
Target *target(const char *output, Cmd *cmd, ...);
Target *obj(const char *obj_file, Cmd *compile_cmd, ...);
Target *link_binary(const char *exe_name, Cmd *link_flags, ...);

void add_dep(Target *t, Target *dep);
void add_deps(Target *t, ...);
bool remove_dep(Target *t, Target *dep);
void clear_deps(Target *t);
bool has_dep(Target *t, Target *dep);
size_t dep_count(Target *t);

void rebuild_self(int argc, char **argv);
void build_target(Target *root);
time_t get_file_mtime(const char *path);

#endif // BUILD_CONFIG_H
