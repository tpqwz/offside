#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "engine.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#define PATH_SEP ';'
#define DIR_SEP '\\'
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#define PATH_SEP ':'
#define DIR_SEP '/'
#endif

#define VERSION "0.1.0-alpha.1"

static const char HELP[] =
  "offside " VERSION "\n"
  "\n"
  "offside run <file> [program args] [-- <compiler args>] [-- <program args>]\n"
  "offside emit <file> [--plain]\n"
  "offside space <file>\n"
  "\n"
  "  .csp .sjava .sjs .spas     spaced source\n"
  "  .c .h .java .js            host C, Java, JavaScript\n"
  "  .pas .pp .lpr .dpr .inc    host Pascal\n"
  "\n"
  "  offside run hello.csp -- -O2\n"
  "  offside emit hello.csp\n"
  "  offside space hello.c\n"
  "  offside run demo.spas\n"
  "\n"
  "  --lang c|java|javascript|pascal   ignore the extension\n"
  "  --cc <compiler>            C compiler, else tcc or cc\n"
  "  --keep                     leave the host file on disk\n"
  "  --plain                    emit host code without #line\n"
  "  --version                  print " VERSION "\n"
  "\n"
  "  emit turns spaced source into host code.\n"
  "  space turns host code into the spaced dialect.\n"
  "  run accepts either, then compiles.\n"
  "\n"
  "  C: tcc defaults to -std=c99. gcc and clang default to -std=gnu2x.\n"
  "  Pascal: Free Pascal (ppcx64). FPC or OFFSIDE_FPC picks the compiler.\n"
  "  OFFSIDE_CC or TCC or CC overrides the C compiler.\n";

typedef struct ArgList {
  const char **items;
  size_t len;
  size_t cap;
} ArgList;

static void args_push(ArgList *list, const char *item) {
  if (list->len + 1u >= list->cap) {
    size_t cap = list->cap == 0 ? 8u : list->cap * 2u;
    list->items = rt_realloc((void *)list->items, cap * sizeof *list->items);
    list->cap = cap;
  }
  list->items[list->len++] = item;
  list->items[list->len] = NULL;
}

static void args_free(ArgList *list) {
  free((void *)list->items);
  list->items = NULL;
  list->len = 0;
  list->cap = 0;
}

typedef enum Command { CMD_RUN, CMD_EMIT, CMD_SPACE, CMD_HELP } Command;

typedef struct Plan {
  Command command;
  const char *file;
  const char *lang;
  const char *cc;
  bool keep;
  bool plain;
  ArgList cc_flags;
  ArgList program_args;
} Plan;

static bool file_exec(const char *path) {
#ifdef _WIN32
  return _access(path, 0) == 0;
#else
  return access(path, X_OK) == 0;
#endif
}

static const char *which_cmd(Arena *arena, const char *cmd) {
  const char *path_env;
  const char *cursor;
  if (cmd == NULL || cmd[0] == '\0') {
    return NULL;
  }
  if (strchr(cmd, '/') != NULL || strchr(cmd, '\\') != NULL) {
    return file_exec(cmd) ? cmd : NULL;
  }
  path_env = getenv("PATH");
  if (path_env == NULL) {
    return NULL;
  }
  cursor = path_env;
  while (*cursor != '\0') {
    const char *end = strchr(cursor, PATH_SEP);
    size_t n = end == NULL ? strlen(cursor) : (size_t)(end - cursor);
    char *candidate;
    if (n > 0) {
      candidate = arena_printf(arena, "%.*s%c%s", (int)n, cursor, DIR_SEP, cmd);
      if (file_exec(candidate)) {
        return candidate;
      }
#ifdef _WIN32
      candidate = arena_printf(arena, "%.*s%c%s.exe", (int)n, cursor, DIR_SEP, cmd);
      if (file_exec(candidate)) {
        return candidate;
      }
#endif
    }
    if (end == NULL) {
      break;
    }
    cursor = end + 1;
  }
  return NULL;
}

static const char *base_only(const char *path) {
  const char *base = path;
  const char *p;
  for (p = path; p != NULL && *p != '\0'; p++) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  return base == NULL ? "" : base;
}

static bool base_is(const char *path, const char *name) {
  return strcmp(base_only(path), name) == 0;
}

static const char *default_c_compiler(Arena *arena) {
  const char *env = getenv("OFFSIDE_CC");
  const char *found;
  if (env == NULL || env[0] == '\0') {
    env = getenv("TCC");
  }
  if (env != NULL && env[0] != '\0') {
    return env;
  }
  found = which_cmd(arena, "tcc");
  if (found != NULL) {
    return found;
  }
  env = getenv("CC");
  if (env != NULL && env[0] != '\0') {
    return env;
  }
  found = which_cmd(arena, "cc");
  if (found != NULL) {
    return found;
  }
  found = which_cmd(arena, "gcc");
  return found != NULL ? found : "cc";
}

static const char *default_pascal_compiler(Arena *arena) {
  const char *env = getenv("OFFSIDE_FPC");
  const char *found;
  if (env == NULL || env[0] == '\0') {
    env = getenv("FPC");
  }
  if (env != NULL && env[0] != '\0') {
    return env;
  }
  if (file_exec("/workspace/.tools/fpc/bin/ppcx64")) {
    return "/workspace/.tools/fpc/bin/ppcx64";
  }
  found = which_cmd(arena, "ppcx64");
  if (found != NULL) {
    return found;
  }
  found = which_cmd(arena, "fpc");
  return found != NULL ? found : "fpc";
}

static void add_implicit_std(ArgList *args, const char *compiler, const ArgList *flags) {
  size_t i;
  const char *base;
  for (i = 0; i < flags->len; i++) {
    if (strcmp(flags->items[i], "-std") == 0 || strncmp(flags->items[i], "-std=", 5) == 0) {
      return;
    }
  }
  base = base_only(compiler);
  if (strcmp(base, "tcc") == 0) {
    args_push(args, "-std=c99");
    return;
  }
  if (strcmp(base, "gcc") == 0 || strcmp(base, "g++") == 0 || strcmp(base, "clang") == 0 || strncmp(base, "clang", 5) == 0) {
    args_push(args, "-std=gnu2x");
    return;
  }
  args_push(args, "-std=c99");
}

static char *read_whole(const char *path, size_t *len, const char **error) {
  FILE *fp = fopen(path, "rb");
  Buf buf;
  char tmp[4096];
  size_t n;
  if (fp == NULL) {
    *error = strerror(errno);
    return NULL;
  }
  buf_init(&buf);
  while ((n = fread(tmp, 1, sizeof tmp, fp)) > 0) {
    buf_append(&buf, tmp, n);
  }
  if (ferror(fp)) {
    *error = strerror(errno);
    buf_free(&buf);
    fclose(fp);
    return NULL;
  }
  fclose(fp);
  *len = buf.len;
  return buf_detach(&buf);
}

static void print_notes(const char *file, const Note *notes, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    fprintf(stderr, "%s:%d: note: %s\n", file, notes[i].line, notes[i].message);
  }
}

static int spawn_mapped(const char *cmd, const ArgList *args, const char *cwd, Arena *arena,
                        const char *generated, const char *source_name, const int *map, size_t nmap, bool remap) {
#ifdef _WIN32
  (void)cwd;
  (void)arena;
  (void)generated;
  (void)source_name;
  (void)map;
  (void)nmap;
  (void)remap;
  {
    const char **argv = rt_malloc_array(args->len + 2u, sizeof(char *));
    size_t i;
    intptr_t code;
    argv[0] = cmd;
    for (i = 0; i < args->len; i++) {
      argv[i + 1u] = args->items[i];
    }
    argv[args->len + 1u] = NULL;
    code = _spawnvp(_P_WAIT, cmd, argv);
    free(argv);
    if (code == -1) {
      fprintf(stderr, "%s: %s\n", cmd, strerror(errno));
      return 127;
    }
    return (int)code;
  }
#else
  int pipefd[2];
  pid_t pid;
  char **argv;
  size_t i;
  int status;
  if (pipe(pipefd) != 0) {
    fprintf(stderr, "%s: %s\n", cmd, strerror(errno));
    return 127;
  }
  argv = rt_malloc_array(args->len + 2u, sizeof(char *));
  argv[0] = (char *)cmd;
  for (i = 0; i < args->len; i++) {
    argv[i + 1u] = (char *)args->items[i];
  }
  argv[args->len + 1u] = NULL;
  pid = fork();
  if (pid < 0) {
    fprintf(stderr, "%s: %s\n", cmd, strerror(errno));
    close(pipefd[0]);
    close(pipefd[1]);
    free(argv);
    return 127;
  }
  if (pid == 0) {
    close(pipefd[0]);
    if (dup2(pipefd[1], STDERR_FILENO) < 0) {
      _exit(127);
    }
    close(pipefd[1]);
    if (cwd != NULL && cwd[0] != '\0' && chdir(cwd) != 0) {
      _exit(127);
    }
    execvp(cmd, argv);
    _exit(127);
  }
  free(argv);
  close(pipefd[1]);
  {
    Buf pending;
    char tmp[1024];
    ssize_t got;
    buf_init(&pending);
    while ((got = read(pipefd[0], tmp, sizeof tmp)) > 0) {
      size_t off = 0;
      buf_append(&pending, tmp, (size_t)got);
      while (off < pending.len) {
        char *nl = memchr(pending.data + off, '\n', pending.len - off);
        size_t end = nl == NULL ? pending.len : (size_t)(nl - pending.data) + 1u;
        char *line;
        char saved = pending.data[end - 1u];
        pending.data[end - 1u] = '\0';
        if (nl == NULL) {
          pending.data[end - 1u] = saved;
          break;
        }
        line = pending.data + off;
        if (remap) {
          char *mapped = remap_diagnostics(arena, line, generated, source_name, map, nmap);
          fputs(mapped, stderr);
        } else {
          fputs(line, stderr);
        }
        fputc('\n', stderr);
        off = end;
      }
      if (off > 0 && off <= pending.len) {
        memmove(pending.data, pending.data + off, pending.len - off);
        pending.len -= off;
        if (pending.data != NULL) {
          pending.data[pending.len] = '\0';
        }
      }
    }
    if (pending.len > 0) {
      if (remap) {
        char *mapped = remap_diagnostics(arena, pending.data, generated, source_name, map, nmap);
        fputs(mapped, stderr);
      } else {
        fputs(pending.data, stderr);
      }
      if (pending.data[pending.len - 1u] != '\n') {
        fputc('\n', stderr);
      }
    }
    buf_free(&pending);
  }
  close(pipefd[0]);
  if (waitpid(pid, &status, 0) < 0) {
    return 1;
  }
  if (WIFSIGNALED(status)) {
    return 1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return 1;
#endif
}

static char *make_temp(void) {
#ifdef _WIN32
  char root[MAX_PATH];
  char *out;
  unsigned n;
  if (GetTempPathA(MAX_PATH, root) == 0) {
    return NULL;
  }
  out = rt_malloc(MAX_PATH);
  for (n = 0; n < 1000u; n++) {
    if (snprintf(out, MAX_PATH, "%soffside-%u", root, GetTickCount() ^ (n * 2654435761u)) >= MAX_PATH) {
      continue;
    }
    if (_mkdir(out) == 0) {
      return out;
    }
  }
  free(out);
  return NULL;
#else
  char *tmpl = rt_malloc(64);
  memcpy(tmpl, "/tmp/offside-XXXXXX", 20);
  if (mkdtemp(tmpl) == NULL) {
    free(tmpl);
    return NULL;
  }
  return tmpl;
#endif
}

static void rm_tree(const char *path) {
#ifdef _WIN32
  (void)path;
#else
  DIR *dir = opendir(path);
  struct dirent *ent;
  if (dir == NULL) {
    unlink(path);
    return;
  }
  while ((ent = readdir(dir)) != NULL) {
    char child[4096];
    struct stat st;
    int wrote;
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }
    wrote = snprintf(child, sizeof child, "%s/%s", path, ent->d_name);
    if (wrote < 0 || (size_t)wrote >= sizeof child) {
      continue;
    }
    if (lstat(child, &st) != 0) {
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      rm_tree(child);
    } else {
      unlink(child);
    }
  }
  closedir(dir);
  rmdir(path);
#endif
}

static const char *parent_dir(Arena *arena, const char *path) {
  const char *slash = NULL;
  const char *p;
  for (p = path; *p != '\0'; p++) {
    if (*p == '/' || *p == '\\') {
      slash = p;
    }
  }
  if (slash == NULL) {
    return ".";
  }
  if (slash == path) {
    return arena_copy(arena, path, 1u);
  }
  return arena_copy(arena, path, (size_t)(slash - path));
}

static bool flag_is(const ArgList *flags, const char *name) {
  size_t i;
  for (i = 0; i < flags->len; i++) {
    if (strcmp(flags->items[i], name) == 0) {
      return true;
    }
  }
  return false;
}

static bool flag_prefix(const ArgList *flags, const char *name) {
  size_t i;
  size_t n = strlen(name);
  for (i = 0; i < flags->len; i++) {
    if (strncmp(flags->items[i], name, n) == 0) {
      return true;
    }
  }
  return false;
}

static const char *output_path(const ArgList *flags) {
  size_t i;
  for (i = 0; i < flags->len; i++) {
    if (strcmp(flags->items[i], "-o") == 0 && i + 1u < flags->len) {
      return flags->items[i + 1u];
    }
    if (strncmp(flags->items[i], "-o", 2) == 0 && flags->items[i][2] != '\0') {
      return flags->items[i] + 2;
    }
  }
  return NULL;
}

static int write_text(const char *path, const char *text) {
  FILE *fp = fopen(path, "wb");
  size_t n;
  if (fp == NULL) {
    fprintf(stderr, "%s: %s\n", path, strerror(errno));
    return 1;
  }
  n = text == NULL ? 0 : strlen(text);
  if (n > 0 && fwrite(text, 1, n, fp) != n) {
    fprintf(stderr, "%s: %s\n", path, strerror(errno));
    fclose(fp);
    return 1;
  }
  if (fclose(fp) != 0) {
    return 1;
  }
  return 0;
}

static const char *pascal_units(Arena *arena, const char *compiler) {
  const char *env = getenv("FPC_UNITS");
  char *sibling;
  if (env != NULL && env[0] != '\0') {
    return env;
  }
  sibling = arena_printf(arena, "%s/../lib/fpc/3.2.2/units/x86_64-linux/rtl", parent_dir(arena, compiler));
  if (file_exec(compiler)) {
    struct stat st;
    if (stat(sibling, &st) == 0) {
      return sibling;
    }
  }
  return "/workspace/.tools/fpc/lib/fpc/3.2.2/units/x86_64-linux/rtl";
}

static int run_tool(const char *compiler, ArgList *args, const char *cwd, Arena *arena, const char *generated,
                    const char *source_name, const HostFile *host, bool report) {
  return spawn_mapped(compiler, args, cwd, arena, generated, source_name,
                      host == NULL ? NULL : host->line_map, host == NULL ? 0 : host->nmap, report);
}

static const char *stem_copy(Arena *arena, const char *file);
static int run_c(Arena *arena, const char *compiler, const char *source_path, const Result *result, const ArgList *cc_flags,
                 const ArgList *program_args, bool keep, bool report) {
  char *dir = make_temp();
  const char *host_name;
  char *host_path;
  HostFile rendered;
  ArgList args;
  int code;
  bool compile_only;
  bool has_out;
  const char *binary;
  const char *explicit_out;
  if (dir == NULL) {
    fputs("offside: cannot make a temporary directory\n", stderr);
    return 1;
  }
  host_name = host_file_name(arena, LANG_C, source_path, result->code);
  host_path = arena_printf(arena, "%s%c%s", dir, DIR_SEP, host_name);
  render_host(arena, result, source_path, report, &rendered);
  if (write_text(host_path, rendered.code) != 0) {
    if (!keep) {
      rm_tree(dir);
    }
    free(dir);
    return 1;
  }
  memset(&args, 0, sizeof args);
  add_implicit_std(&args, compiler, cc_flags);
  args_push(&args, "-I");
  args_push(&args, parent_dir(arena, source_path));
  {
    size_t i;
    for (i = 0; i < cc_flags->len; i++) {
      args_push(&args, cc_flags->items[i]);
    }
  }
  compile_only = flag_is(cc_flags, "-c") || flag_is(cc_flags, "-E") || flag_is(cc_flags, "-S");
  has_out = flag_is(cc_flags, "-o") || flag_prefix(cc_flags, "-o");
  if (base_is(compiler, "tcc") && !compile_only && !has_out) {
    args_push(&args, "-run");
    args_push(&args, host_path);
    {
      size_t i;
      for (i = 0; i < program_args->len; i++) {
        args_push(&args, program_args->items[i]);
      }
    }
    code = run_tool(compiler, &args, NULL, arena, host_path, source_path, &rendered, report);
  } else {
    binary = arena_printf(arena, "%s%c%s", dir, DIR_SEP, stem_copy(arena, host_name));
    args_push(&args, host_path);
    if (!compile_only && !has_out) {
      args_push(&args, "-o");
      args_push(&args, binary);
    }
    code = run_tool(compiler, &args, NULL, arena, host_path, source_path, &rendered, report);
    if (code == 0 && !compile_only) {
      explicit_out = has_out ? output_path(cc_flags) : binary;
      if (explicit_out != NULL) {
        ArgList run_args;
        size_t i;
        memset(&run_args, 0, sizeof run_args);
        for (i = 0; i < program_args->len; i++) {
          args_push(&run_args, program_args->items[i]);
        }
        code = run_tool(explicit_out, &run_args, NULL, arena, host_path, source_path, &rendered, report);
        args_free(&run_args);
      }
    }
  }
  args_free(&args);
  if (keep) {
    fprintf(stderr, "kept %s\n", host_path);
  } else {
    rm_tree(dir);
  }
  free(dir);
  return code;
}

/* stem without extension, for the binary sitting next to the host file. */
static const char *stem_copy(Arena *arena, const char *file) {
  const char *base = base_only(file);
  const char *dot = strrchr(base, '.');
  if (dot == NULL || dot == base) {
    return arena_cstr(arena, base);
  }
  return arena_copy(arena, base, (size_t)(dot - base));
}

static int run_pascal(Arena *arena, const char *compiler, const char *source_path, const Result *result,
                      const ArgList *cc_flags, const ArgList *program_args, bool keep, bool report) {
  char *dir = make_temp();
  const char *host_name;
  char *host_path;
  HostFile rendered;
  ArgList args;
  int code;
  bool compile_only;
  bool has_out;
  const char *binary;
  if (dir == NULL) {
    return 1;
  }
  host_name = host_file_name(arena, LANG_PASCAL, source_path, result->code);
  host_path = arena_printf(arena, "%s%c%s", dir, DIR_SEP, host_name);
  render_host(arena, result, source_path, false, &rendered);
  if (write_text(host_path, rendered.code) != 0) {
    free(dir);
    return 1;
  }
  memset(&args, 0, sizeof args);
  args_push(&args, arena_printf(arena, "-Fu%s", pascal_units(arena, compiler)));
  args_push(&args, arena_printf(arena, "-Fu%s", parent_dir(arena, source_path)));
  {
    size_t i;
    for (i = 0; i < cc_flags->len; i++) {
      args_push(&args, cc_flags->items[i]);
    }
  }
  args_push(&args, host_path);
  compile_only = flag_is(cc_flags, "-c");
  has_out = flag_is(cc_flags, "-o") || flag_prefix(cc_flags, "-o");
  binary = arena_printf(arena, "%s%c%s", dir, DIR_SEP, stem_copy(arena, host_name));
  if (!compile_only && !has_out) {
    args_push(&args, arena_printf(arena, "-o%s", binary));
  }
  code = run_tool(compiler, &args, dir, arena, host_path, source_path, &rendered, report);
  if (code == 0 && !compile_only) {
    const char *explicit_out = has_out ? output_path(cc_flags) : binary;
    if (explicit_out != NULL) {
      ArgList run_args;
      size_t i;
      memset(&run_args, 0, sizeof run_args);
      for (i = 0; i < program_args->len; i++) {
        args_push(&run_args, program_args->items[i]);
      }
      code = run_tool(explicit_out, &run_args, dir, arena, host_path, source_path, &rendered, report);
      args_free(&run_args);
    }
  }
  args_free(&args);
  if (keep) {
    fprintf(stderr, "kept %s\n", host_path);
  } else {
    rm_tree(dir);
  }
  free(dir);
  return code;
}

static int run_java(Arena *arena, const char *source_path, const Result *result, const ArgList *cc_flags,
                    const ArgList *program_args, bool keep, bool report) {
  char *dir = make_temp();
  const char *host_name;
  char *host_path;
  HostFile rendered;
  ArgList args;
  int code;
  const char *class_name;
  if (dir == NULL) {
    return 1;
  }
  host_name = host_file_name(arena, LANG_JAVA, source_path, result->code);
  host_path = arena_printf(arena, "%s%c%s", dir, DIR_SEP, host_name);
  render_host(arena, result, source_path, false, &rendered);
  if (write_text(host_path, rendered.code) != 0) {
    free(dir);
    return 1;
  }
  memset(&args, 0, sizeof args);
  {
    size_t i;
    for (i = 0; i < cc_flags->len; i++) {
      args_push(&args, cc_flags->items[i]);
    }
  }
  args_push(&args, host_path);
  code = run_tool("javac", &args, NULL, arena, host_path, source_path, &rendered, report);
  args_free(&args);
  if (code == 0 && !flag_is(cc_flags, "-c")) {
    ArgList run_args;
    size_t i;
    const char *dot = strrchr(host_name, '.');
    class_name = dot == NULL ? host_name : arena_copy(arena, host_name, (size_t)(dot - host_name));
    memset(&run_args, 0, sizeof run_args);
    args_push(&run_args, "-cp");
    args_push(&run_args, dir);
    args_push(&run_args, class_name);
    for (i = 0; i < program_args->len; i++) {
      args_push(&run_args, program_args->items[i]);
    }
    code = run_tool("java", &run_args, NULL, arena, host_path, source_path, &rendered, report);
    args_free(&run_args);
  }
  if (keep) {
    fprintf(stderr, "kept %s\n", host_path);
  } else {
    rm_tree(dir);
  }
  free(dir);
  return code;
}

static int run_javascript(Arena *arena, const char *source_path, const Result *result, const ArgList *cc_flags,
                          const ArgList *program_args, bool keep, bool report) {
  char *dir = make_temp();
  char *host_path;
  HostFile rendered;
  ArgList args;
  int code;
  size_t i;
  if (dir == NULL) {
    return 1;
  }
  host_path = arena_printf(arena, "%s%c%s", dir, DIR_SEP, host_file_name(arena, LANG_JAVASCRIPT, source_path, result->code));
  render_host(arena, result, source_path, false, &rendered);
  if (write_text(host_path, rendered.code) != 0) {
    free(dir);
    return 1;
  }
  memset(&args, 0, sizeof args);
  for (i = 0; i < cc_flags->len; i++) {
    args_push(&args, cc_flags->items[i]);
  }
  args_push(&args, host_path);
  for (i = 0; i < program_args->len; i++) {
    args_push(&args, program_args->items[i]);
  }
  code = run_tool("node", &args, NULL, arena, host_path, source_path, &rendered, report);
  args_free(&args);
  if (keep) {
    fprintf(stderr, "kept %s\n", host_path);
  } else {
    rm_tree(dir);
  }
  free(dir);
  return code;
}

static int parse_plan(int argc, char **argv, Plan *plan, const char **error) {
  int i;
  int groups = 1;
  Command command;
  memset(plan, 0, sizeof *plan);
  if (argc <= 0 || strcmp(argv[0], "--help") == 0 || strcmp(argv[0], "-h") == 0) {
    plan->command = CMD_HELP;
    return 0;
  }
  if (strcmp(argv[0], "run") == 0) {
    command = CMD_RUN;
  } else if (strcmp(argv[0], "emit") == 0) {
    command = CMD_EMIT;
  } else if (strcmp(argv[0], "space") == 0) {
    command = CMD_SPACE;
  } else {
    *error = "unknown";
    return 2;
  }
  plan->command = command;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--") == 0 && groups < 3) {
      groups++;
      continue;
    }
    if (groups == 1) {
      if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
        plan->command = CMD_HELP;
        return 0;
      }
      if (strcmp(argv[i], "--keep") == 0 || strcmp(argv[i], "-k") == 0) {
        plan->keep = true;
        continue;
      }
      if (strcmp(argv[i], "--plain") == 0) {
        plan->plain = true;
        continue;
      }
      if (strcmp(argv[i], "--lang") == 0 || strcmp(argv[i], "-l") == 0) {
        if (i + 1 >= argc) {
          *error = "--lang needs c, java, javascript, or pascal.";
          return 2;
        }
        plan->lang = argv[++i];
        continue;
      }
      if (strcmp(argv[i], "--cc") == 0) {
        if (i + 1 >= argc) {
          *error = "--cc needs a compiler path.";
          return 2;
        }
        plan->cc = argv[++i];
        continue;
      }
      if (argv[i][0] == '-') {
        *error = "unknown-option";
        return 2;
      }
      if (plan->file == NULL) {
        plan->file = argv[i];
      } else {
        args_push(&plan->program_args, argv[i]);
      }
    } else if (groups == 2) {
      args_push(&plan->cc_flags, argv[i]);
    } else {
      args_push(&plan->program_args, argv[i]);
    }
  }
  if (plan->file == NULL) {
    *error = "Missing a source file.";
    return 2;
  }
  if ((command == CMD_EMIT || command == CMD_SPACE) && (plan->cc_flags.len > 0 || plan->program_args.len > 0)) {
    *error = "flags";
    return 2;
  }
  return 0;
}

static int execute(Plan *plan) {
  Arena *scratch;
  SourceKind kind;
  char *source;
  size_t len = 0;
  const char *read_error = NULL;
  Result laid;
  int code = 0;
  if (plan->command == CMD_HELP || plan->file == NULL) {
    fputs(HELP, stdout);
    return 0;
  }
  scratch = arena_new();
  kind = classify_file(plan->file, plan->lang);
  if (!kind.ok) {
    fprintf(stderr, "%s\n", kind.error);
    arena_free(scratch);
    return 2;
  }
  source = read_whole(plan->file, &len, &read_error);
  if (source == NULL) {
    fprintf(stderr, "%s\n", read_error == NULL ? "cannot read file" : read_error);
    arena_free(scratch);
    return 1;
  }
  if (plan->command == CMD_SPACE) {
    Result spaced = to_spaced(source, len, kind.lang);
    print_notes(plan->file, spaced.notes, spaced.nnotes);
    fputs(spaced.code == NULL ? "" : spaced.code, stdout);
    result_free(&spaced);
    free(source);
    arena_free(scratch);
    return 0;
  }
  if (kind.host && plan->command == CMD_EMIT) {
    fprintf(stderr, "%s is host source. offside space writes the spaced form.\n", plan->file);
    free(source);
    arena_free(scratch);
    return 2;
  }
  if (kind.host) {
    Result spaced = to_spaced(source, len, kind.lang);
    print_notes(plan->file, spaced.notes, spaced.nnotes);
    free(source);
    source = rt_malloc(strlen(spaced.code) + 1u);
    memcpy(source, spaced.code, strlen(spaced.code) + 1u);
    len = strlen(source);
    result_free(&spaced);
  }
  laid = layout_source(source, len, kind.lang);
  print_notes(plan->file, laid.notes, laid.nnotes);
  if (plan->command == CMD_EMIT) {
    HostFile host;
    render_host(laid.arena, &laid, plan->file, !plan->plain, &host);
    fputs(host.code, stdout);
    result_free(&laid);
    free(source);
    arena_free(scratch);
    return 0;
  }
  if (kind.lang == LANG_C) {
    code = run_c(scratch, plan->cc != NULL ? plan->cc : default_c_compiler(scratch), plan->file, &laid,
                 &plan->cc_flags, &plan->program_args, plan->keep, !kind.host);
  } else if (kind.lang == LANG_JAVA) {
    code = run_java(scratch, plan->file, &laid, &plan->cc_flags, &plan->program_args, plan->keep, !kind.host);
  } else if (kind.lang == LANG_PASCAL) {
    code = run_pascal(scratch, plan->cc != NULL ? plan->cc : default_pascal_compiler(scratch), plan->file, &laid,
                      &plan->cc_flags, &plan->program_args, plan->keep, !kind.host);
  } else {
    code = run_javascript(scratch, plan->file, &laid, &plan->cc_flags, &plan->program_args, plan->keep, !kind.host);
  }
  result_free(&laid);
  free(source);
  arena_free(scratch);
  return code;
}

int main(int argc, char **argv) {
  Plan plan;
  const char *error = NULL;
  int parsed;
  if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
    fputs(VERSION "\n", stdout);
    return 0;
  }
  parsed = parse_plan(argc - 1, argv + 1, &plan, &error);
  if (parsed != 0) {
    if (error != NULL && strcmp(error, "unknown") == 0) {
      fprintf(stderr, "Unknown command %s.\n%s", argv[1], HELP);
    } else if (error != NULL && strcmp(error, "unknown-option") == 0) {
      fprintf(stderr, "Unknown option.\n%s", HELP);
    } else if (error != NULL && strcmp(error, "flags") == 0) {
      fprintf(stderr, "%s only writes source. Compiler flags belong on run.\n%s", argv[1], HELP);
    } else if (error != NULL) {
      fprintf(stderr, "%s\n%s", error, HELP);
    }
    args_free(&plan.cc_flags);
    args_free(&plan.program_args);
    return 2;
  }
  parsed = execute(&plan);
  args_free(&plan.cc_flags);
  args_free(&plan.program_args);
  return parsed;
}
