// RegexPattern::compile() with each of its allocations failing in turn, one
// per child process. A crash is an error; a crashing child prints its stack.
// A compilation that reports success but gives a pattern that does not match
// is printed with the stack of the failed allocation, and is not an error
// here. Only the allocations of the compilation fail: the match that checks
// the pattern runs without failures.
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <unicode/regex.h>
#include <unicode/uclean.h>
#include <unicode/unistr.h>

using icu::RegexMatcher;
using icu::RegexPattern;
using icu::UnicodeString;

static long counter;
static long fail_at = -1;
static long compile_allocations;  // made by the last compilation
static void *failed_stack[64];     // where the failed allocation was made
static int failed_depth;

static void *U_CALLCONV fail_alloc(const void *, size_t size) {
  if (++counter == fail_at) {
    failed_depth = backtrace(failed_stack, 64);
    return nullptr;
  }
  return malloc(size);
}

static void *U_CALLCONV fail_realloc(const void *, void *p, size_t size) {
  if (++counter == fail_at) {
    failed_depth = backtrace(failed_stack, 64);
    return nullptr;
  }
  return realloc(p, size);
}

static void U_CALLCONV plain_free(const void *, void *p) { free(p); }

static void print_stack(int sig) {
  void *frames[64];
  int n = backtrace(frames, 64);
  backtrace_symbols_fd(frames, n, STDOUT_FILENO);
  signal(sig, SIG_DFL);
  raise(sig);
}

static UnicodeString const pattern(u"(a|[b-d])+x\\d{2,5}");
static UnicodeString const subject(u"zzabcx123");

// Compiles the pattern with the allocation numbered fail failing (none for
// -1); true if it fails with an error status or the pattern matches.
static bool compile(long fail) {
  UErrorCode status = U_ZERO_ERROR;
  UParseError pe;
  counter = 0;
  fail_at = fail;
  RegexPattern *p = RegexPattern::compile(pattern, 0, pe, status);
  fail_at = -1;
  compile_allocations = counter;
  if (U_FAILURE(status))
    return p == nullptr;
  RegexMatcher *m = p->matcher(subject, status);
  bool const found = U_SUCCESS(status) && m->find(status) && U_SUCCESS(status);
  delete m;
  delete p;
  return found;
}

int main() {
  UErrorCode status = U_ZERO_ERROR;
  u_setMemoryFunctions(nullptr, fail_alloc, fail_realloc, plain_free, &status);
  if (U_FAILURE(status)) {
    printf("u_setMemoryFunctions: %s\n", u_errorName(status));
    return 1;
  }
  // The first compilation loads the data and fills caches.
  if (!compile(-1) || !compile(-1)) {
    printf("no match without a failed allocation\n");
    return 1;
  }
  long const total = compile_allocations;
  int crashed = 0, wrong = 0;
  for (long n = 1; n <= total; n++) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      return 1;
    }
    if (pid == 0) {
      signal(SIGSEGV, print_stack);
      signal(SIGBUS, print_stack);
      signal(SIGABRT, print_stack);
      if (compile(n))
        _exit(0);
      printf("allocation %ld of %ld failed: success without a match; the"
             " failed allocation:\n", n, total);
      fflush(stdout);
      backtrace_symbols_fd(failed_stack, failed_depth, STDOUT_FILENO);
      _exit(1);
    }
    int st;
    if (waitpid(pid, &st, 0) != pid) {
      perror("waitpid");
      return 1;
    }
    if (WIFSIGNALED(st)) {
      printf("allocation %ld of %ld failed: signal %d\n", n, total,
             WTERMSIG(st));
      crashed++;
    } else if (WEXITSTATUS(st) != 0) {
      wrong++;
    }
  }
  printf("RegexPattern::compile: %ld allocations, %d crashed, %d without a"
         " match\n", total, crashed, wrong);
  return crashed != 0;
}
