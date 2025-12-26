#include "build.config.h"
#include <stdbool.h>

int main(int argc, char **argv) {
  rebuild_self(argc, argv);

  const char *exe_sources[] = {"src/game.c", "src/patcher.c", "src/main.c",
                               "src/msg.c", NULL};
  BINARY(exe,                      // Target variable name
         "rivals_sig_patcher.exe", // Output file path
         exe_sources,              // NULL-terminated source file array
         NULL,                     // Dependencies (use DEPS() or NULL)
         "-Iinclude",              // Include flags
         NULL,                     // Linker command (NULL = use CC)
         "-mconsole");             // LDFLAGS

  build_target(exe);
  return 0;
}

time_t get_file_mtime(const char *path) {
  struct stat attr;
  if (stat(path, &attr) < 0)
    return 0;
  return attr.st_mtime;
}

void cmd_internal_append(Cmd *cmd, ...);
#define cmd_append(CMD, ...) cmd_internal_append(CMD, __VA_ARGS__, NULL)

void cmd_print(const Cmd *cmd);
int cmd_run(Cmd *cmd, const char *task_out, int task_num, size_t total_cmd);

bool need_self_rebuild(char *binpath) {
  const char *source_path = __FILE__;
  const char *binary_path = binpath;

  // Build config file path: strip .c and add .config.h
  size_t src_len = strlen(source_path);
  char config_path[1024];
  if (src_len > 2 && strcmp(source_path + src_len - 2, ".c") == 0) {
    snprintf(config_path, sizeof(config_path), "%.*s.config.h",
             (int)(src_len - 2), source_path);
  } else {
    snprintf(config_path, sizeof(config_path), "%s.config.h", source_path);
  }

  time_t binary_mtime = get_file_mtime(binary_path);
  time_t source_mtime = get_file_mtime(source_path);
  time_t config_mtime = get_file_mtime(config_path);

  // Rebuild if source or config is newer than binary
  if (source_mtime <= binary_mtime && config_mtime <= binary_mtime)
    return false;

  return true;
}
void rebuild_self(int argc, char **argv) {
  if (!need_self_rebuild(argv[0])) {
    return;
  }
#if VERBOSE_BUILD_INFO
  printf("[INFO] Rebuilding build system...\n");
#endif

  char old_binary_path[1024];
  snprintf(old_binary_path, sizeof(old_binary_path), "%s.old", argv[0]);

#ifdef _WIN32
  if (get_file_mtime(old_binary_path) != 0) {
    remove(old_binary_path);
  }
#endif

  if (rename(argv[0], old_binary_path) != 0) {
    panic("Could not rename old binary");
  }

  Cmd cmd = {0};
  cmd_append(&cmd, SELF_CC, __FILE__, "-o", argv[0]);

  if (!cmd_run(&cmd, "build.c", 0, 0)) {
    rename(old_binary_path, argv[0]);
    panic("Failed to rebuild build system");
  }
  unlink(old_binary_path);

#if VERBOSE_BUILD_INFO
  printf("[INFO] Restarting %s...\n", argv[0]);
#endif

  execvp(argv[0], argv);
  panic("Failed to exec rebuilt binary");
}

void cmd_internal_append(Cmd *cmd, ...) {
  if (cmd == NULL)
    panic("cmd is a null pointer");

  va_list args;
  va_start(args, cmd);

  const char *cur_arg = va_arg(args, const char *);

  while (cur_arg != NULL) {
    if (cmd->count == cmd->capacity) {
      size_t newsize = (cmd->capacity == 0) ? 10 : cmd->capacity * 2;
      const char **new_items =
          (const char **)realloc(cmd->args, newsize * sizeof(char *));

      if (!new_items)
        panic("Out of memory");

      cmd->args = new_items;
      cmd->capacity = newsize;
    }

    cmd->args[cmd->count++] = cur_arg;
    cur_arg = va_arg(args, const char *);
  }

  va_end(args);
}

void cmd_print(const Cmd *cmd) {
  if (cmd->count == 0)
    return;

  for (size_t i = 0; i < cmd->count; ++i) {
    if (strchr(cmd->args[i], ' ')) {
      printf("'%s'", cmd->args[i]);
    } else {
      printf("%s", cmd->args[i]);
    }

    if (i + 1 < cmd->count)
      printf(" ");
  }
  printf("\n");
  fflush(stdout);
}

int cmd_run(Cmd *cmd, const char *task_out, int task_num, size_t total_cmd) {
  if (cmd->count == 0)
    return 1;

  if (total_cmd > 0) {
    printf("[CC %d/%zu] %s\n", task_num, total_cmd, task_out);
  }

#if PRINT_COMPILATION_COMMANDS
  printf("\t");
  cmd_print(cmd);
#endif

  pid_t pid = fork();
  if (pid < 0)
    panic("fork failed");

  if (pid == 0) {
    execvp(cmd->args[0], (char *const *)cmd->args);
    panic("Failed to execvp");
  }

  int status;
  waitpid(pid, &status, 0);

  if (WIFEXITED(status)) {
    int exit_code = WEXITSTATUS(status);
    if (exit_code != 0) {
      fprintf(stderr, "Command failed with exit code %d\n", exit_code);
      return 0;
    }
    return 1;
  }

  if (WIFSIGNALED(status))
    panic("Build terminated with signal %d", WTERMSIG(status));

  return 0;
}

void q_push(Queue *q, Target *t) {
  pthread_mutex_lock(&q->mutex);

  if (q->size == q->cap)
    panic("Max queue size reached");

  q->buf[q->tail] = t;
  q->tail = (q->tail + 1) % q->cap;
  q->size++;

  pthread_cond_signal(&q->cond);
  pthread_mutex_unlock(&q->mutex);
}

Target *queue_pop(Queue *q) {
  pthread_mutex_lock(&q->mutex);

  while (q->size == 0 && !q->done) {
    pthread_cond_wait(&q->cond, &q->mutex);
  }

  if (q->size == 0 && q->done) {
    pthread_mutex_unlock(&q->mutex);
    return NULL;
  }

  Target *t = q->buf[q->head];
  q->head = (q->head + 1) % q->cap;
  q->size--;

  pthread_mutex_unlock(&q->mutex);
  return t;
}

void add_parent(Target *child, Target *parent) {
  pthread_mutex_lock(&child->lock);

  if (child->parent_cnt == child->parent_cap) {
    child->parent_cap = (child->parent_cap == 0) ? 4 : child->parent_cap * 2;
    child->parents =
        realloc(child->parents, child->parent_cap * sizeof(Target *));
    if (!child->parents)
      panic("Out of memory allocating parent array");
  }

  child->parents[child->parent_cnt++] = parent;
  pthread_mutex_unlock(&child->lock);
}

void analyze_graph(Target *t, size_t *total_nodes, size_t *cnt_cmd) {
  if (atomic_exchange(&t->visited, true))
    return;

  (*total_nodes)++;
  if (t->cmd)
    (*cnt_cmd)++;

  pthread_mutex_init(&t->lock, NULL);
  t->parents = NULL;
  t->parent_cnt = 0;
  t->parent_cap = 0;

  int dep_cnt = 0;

  if (t->deps) {
    for (int i = 0; t->deps[i] != NULL; i++) {
      Target *dep = t->deps[i];
      analyze_graph(dep, total_nodes, cnt_cmd);
      add_parent(dep, t);
      dep_cnt++;
    }
  }

  atomic_store(&t->pending_deps, dep_cnt);
}

void reset_target_state(Target *t) {
  bool expected = true;
  if (!atomic_compare_exchange_strong(&t->visited, &expected, false))
    return;

  if (t->parents) {
    free(t->parents);
    t->parents = NULL;
    t->parent_cnt = 0;
    t->parent_cap = 0;
  }

  if (t->lock.__data.__kind != 0) {
    pthread_mutex_destroy(&t->lock);
  }

  atomic_store(&t->pending_deps, 0);

  if (t->deps) {
    for (int i = 0; t->deps[i]; i++) {
      reset_target_state(t->deps[i]);
    }
  }
}

void push_ready_nodes(Target *t, Queue *q) {
  bool expected = true;
  if (!atomic_compare_exchange_strong(&t->visited, &expected, false))
    return;

  if (atomic_load(&t->pending_deps) == 0) {
    q_push(q, t);
  }

  if (t->deps) {
    for (int i = 0; t->deps[i]; i++)
      push_ready_nodes(t->deps[i], q);
  }
}

void *worker(void *arg) {
  Queue *q = (Queue *)arg;

  const char *source_path = __FILE__;
  size_t src_len = strlen(source_path);
  char config_path[1024];
  if (src_len > 2 && strcmp(source_path + src_len - 2, ".c") == 0) {
    snprintf(config_path, sizeof(config_path), "%.*s.config.h",
             (int)(src_len - 2), source_path);
  } else {
    snprintf(config_path, sizeof(config_path), "%s.config.h", source_path);
  }
  time_t build_mtime = get_file_mtime(__FILE__);
  time_t config_mtime = get_file_mtime(config_path);

  while (true) {
    Target *t = queue_pop(q);
    if (!t)
      break;

    int needs_rebuild = 0;
    time_t mtime = get_file_mtime(t->output);

    if (mtime == 0)
      needs_rebuild = 1;

    if (t->cmd && (build_mtime > mtime || config_mtime > mtime))
      needs_rebuild = 1;
    if (t->deps) {
      for (int i = 0; t->deps[i] != NULL; i++) {
        Target *dep = t->deps[i];
        if (get_file_mtime(dep->output) > mtime)
          needs_rebuild = 1;
      }
    }

    if (t->cmd) {
      int task_num = atomic_fetch_add(&completed_cmds, 1) + 1;

      if (needs_rebuild) {
        if (!cmd_run(t->cmd, t->output, task_num, q->total_cmd))
          panic("Failed to build %s", t->output);
      } else {
#if SHOW_UP_TO_DATE_MESSAGES
        printf("[%d/%zu] %s (up to date)\n", task_num, q->total_cmd, t->output);
#endif
      }
    }

    if (t->parents) {
      for (size_t i = 0; i < t->parent_cnt; i++) {
        Target *p = t->parents[i];
        if (atomic_fetch_sub(&p->pending_deps, 1) == 1) {
          q_push(q, p);
        }
      }
    }
  }

  return NULL;
}

void build_target(Target *root) {
  long nproc = sysconf(_SC_NPROCESSORS_ONLN);
  if (MAX_THREADS < 1) {
#undef MAX_THREADS
#define MAX_THREADS nproc
    printf("Using %ld threads to compile.\n", nproc);
  }
  size_t cnt_cmd = 0;

  pthread_mutex_lock(&build_lock);
  atomic_store(&completed_cmds, 0);
  total_nodes = 0;

  reset_target_state(root);
  analyze_graph(root, &total_nodes, &cnt_cmd);

  Queue q = {0};
  q.cap = total_nodes + 16;
  q.total_cmd = cnt_cmd;
  q.buf = malloc(sizeof(Target *) * q.cap);

  if (!q.buf)
    panic("Out of memory");

  pthread_mutex_init(&q.mutex, NULL);
  pthread_cond_init(&q.cond, NULL);
  atomic_init(&q.done, false);

  pthread_t threads[MAX_THREADS];
  for (int i = 0; i < MAX_THREADS; i++) {
    pthread_create(&threads[i], NULL, worker, &q);
  }

  push_ready_nodes(root, &q);

  while (atomic_load(&completed_cmds) < cnt_cmd) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000}; // 1ms
    nanosleep(&ts, NULL);
  }

  q.done = true;
  pthread_cond_broadcast(&q.cond);

  for (int i = 0; i < MAX_THREADS; i++) {
    pthread_join(threads[i], NULL);
  }

  free(q.buf);
  pthread_mutex_destroy(&q.mutex);
  pthread_cond_destroy(&q.cond);

  pthread_mutex_unlock(&build_lock);
}

// Helper functions for BINARY macro
static inline char *obj_name_from_src(const char *src) {
  const char *last_slash = strrchr(src, '/');
  const char *basename = last_slash ? last_slash + 1 : src;
  const char *dot = strrchr(basename, '.');
  size_t len = dot ? (size_t)(dot - basename) : strlen(basename);
  char *obj = malloc(len + 3);
  memcpy(obj, basename, len);
  obj[len] = '.';
  obj[len + 1] = 'o';
  obj[len + 2] = '\0';
  return obj;
}

static inline Target *create_source_target(const char *src) {
  Target *t = malloc(sizeof(Target));
  memset(t, 0, sizeof(Target));
  t->output = src;
  t->deps = NULL;
  t->cmd = NULL;
  return t;
}

static inline Target *create_obj_target(const char *src, Target **extra_deps,
                                        const char *compiler,
                                        const char *includes) {
  Target *obj = malloc(sizeof(Target));
  memset(obj, 0, sizeof(Target));
  obj->output = obj_name_from_src(src);

  // Count extra deps
  size_t dep_cnt = 1;
  if (extra_deps) {
    while (extra_deps[dep_cnt - 1])
      dep_cnt++;
  }

  // Build deps array
  Target **deps = malloc(sizeof(Target *) * (dep_cnt + 1));
  deps[0] = create_source_target(src);
  if (extra_deps) {
    memcpy(deps + 1, extra_deps, sizeof(Target *) * (dep_cnt - 1));
  }
  deps[dep_cnt] = NULL;
  obj->deps = deps;

  // Build compile command
  Cmd *cmd = malloc(sizeof(Cmd));
  memset(cmd, 0, sizeof(Cmd));
  cmd_append(cmd, compiler, includes, "-c", src, "-o", obj->output);
  obj->cmd = cmd;

  return obj;
}

static inline Target *create_link_target(const char *output, Target **objs,
                                         size_t obj_count, const char *linker,
                                         const char *ldflags) {
  Target *exe = malloc(sizeof(Target));
  memset(exe, 0, sizeof(Target));
  exe->output = output;

  // Set deps to object files
  Target **deps = malloc(sizeof(Target *) * (obj_count + 1));
  memcpy(deps, objs, sizeof(Target *) * obj_count);
  deps[obj_count] = NULL;
  exe->deps = deps;

  // Use CC if linker not specified
  const char *link_cmd = (linker && strlen(linker) > 0) ? linker : CC;

  Cmd *cmd = malloc(sizeof(Cmd));
  memset(cmd, 0, sizeof(Cmd));
  cmd_append(cmd, link_cmd);
  for (size_t i = 0; i < obj_count; i++) {
    cmd_append(cmd, objs[i]->output);
  }
  cmd_append(cmd, "-o", output);
  if (ldflags && strlen(ldflags) > 0) {
    cmd_append(cmd, ldflags);
  }
  exe->cmd = cmd;

  return exe;
}
