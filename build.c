#define _XOPEN_SOURCE 700
#include "build.config.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

static pthread_mutex_t build_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int completed_cmds = 0;
static size_t total_nodes = 0;


int main(int argc, char **argv) {
    rebuild_self(argc, argv);

    return 0;
}

Cmd* cmd_new(void) {
    Cmd *cmd = calloc(1, sizeof(Cmd));
    cmd->capacity = 8;
    cmd->args = malloc(cmd->capacity * sizeof(char*));
    return cmd;
}

time_t get_file_mtime(const char *path) {
  struct stat attr;
  if (stat(path, &attr) < 0)
    return 0;
  return attr.st_mtime;
}

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

  Cmd *cmd = CMD( SELF_CC, __FILE__, "-o", argv[0], "-lpthread");
  
  if (!cmd_run(cmd, "build.c", 0, 0)) {
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
        panic("get more ram");

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

static void add_parent(Target *child, Target *parent) {
    pthread_mutex_lock(&child->lock); 
    
    if (child->parent_cnt == child->parent_cap) {
        child->parent_cap = (child->parent_cap == 0) ? 4 : child->parent_cap * 2;
        child->parents = realloc(child->parents, child->parent_cap * sizeof(Target *));
        if (!child->parents) panic("Out of memory allocating parent array");
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

  if (t->lock.__data.__kind != 0) { // very hacky
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




int cmd_run(Cmd *cmd, const char *task_out, int task_num, size_t total_cmd);

static inline char* str_clone(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *clone = malloc(len + 1);
    memcpy(clone, s, len + 1);
    return clone;
}

void cmd_extend(Cmd *cmd1, const Cmd *cmd2) {
    if (!cmd1 || !cmd2) panic("cmd_extend: null pointer");
    
    for (size_t i = 0; i < cmd2->count; i++) {
        if (cmd1->count >= cmd1->capacity) {
            cmd1->capacity = cmd1->capacity * 2;
            cmd1->args = realloc(cmd1->args, cmd1->capacity * sizeof(char*));
        }
        cmd1->args[cmd1->count++] = str_clone(cmd2->args[i]);
    }
}

Cmd* cmd_clone(const Cmd *src) {
    if (!src) return NULL;
    
    Cmd *dst = calloc(1, sizeof(Cmd));
    dst->capacity = src->count + 4;
    dst->args = malloc(dst->capacity * sizeof(char*));
    
    for (size_t i = 0; i < src->count; i++) {
        dst->args[dst->count++] = str_clone(src->args[i]);
    }
    
    return dst;
}



Target* src(const char *path) {
    Target *t = calloc(1, sizeof(Target));
    t->output = path;
    return t;
}

Target* target(const char *output, Cmd *cmd, ...) {
    Target *t = calloc(1, sizeof(Target));
    t->output = output;
    t->cmd = cmd;
    size_t dep_count = 0;
    va_list args;
    va_start(args, cmd);
    while (va_arg(args, Target*) != NULL) dep_count++;
    va_end(args);
    
    if (dep_count > 0) {
        t->deps = malloc((dep_count + 1) * sizeof(Target*));
        va_start(args, cmd);
        for (size_t i = 0; i < dep_count; i++) {
            t->deps[i] = va_arg(args, Target*);
        }
        va_end(args);
        t->deps[dep_count] = NULL;
    }
    
    return t;
}

Target* obj(const char *obj_file, Cmd *compile_cmd, ...) {
    const char *dot = strrchr(obj_file, '.');
    size_t base_len = dot ? (size_t)(dot - obj_file) : strlen(obj_file);
    
    char *src_file = malloc(base_len + 3);
    memcpy(src_file, obj_file, base_len);
    strcpy(src_file + base_len, ".c");
    
    // Count extra deps
    size_t extra_count = 0;
    va_list args;
    va_start(args, compile_cmd);
    while (va_arg(args, Target*) != NULL) extra_count++;
    va_end(args);
    
    Target *t = calloc(1, sizeof(Target));
    t->output = obj_file;
    t->cmd = compile_cmd;
    
    // Deps = [source, ...extras]
    t->deps = malloc((extra_count + 2) * sizeof(Target*));
    t->deps[0] = src(src_file);
    
    if (extra_count > 0) {
        va_start(args, compile_cmd);
        for (size_t i = 0; i < extra_count; i++) {
            t->deps[i + 1] = va_arg(args, Target*);
        }
        va_end(args);
    }
    t->deps[extra_count + 1] = NULL;
    
    return t;
}

