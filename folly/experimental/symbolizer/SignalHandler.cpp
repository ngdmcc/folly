/*
 * Copyright 2015 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// This is heavily inspired by the signal handler from google-glog

#include <folly/experimental/symbolizer/SignalHandler.h>

#include <sys/types.h>
#include <sys/syscall.h>
#include <atomic>
#include <ctime>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <vector>

#include <glog/logging.h>

#include <folly/Conv.h>
#include <folly/FileUtil.h>
#include <folly/Portability.h>
#include <folly/ScopeGuard.h>
#include <folly/experimental/symbolizer/Symbolizer.h>

namespace folly { namespace symbolizer {

namespace {

/**
 * Fatal signal handler registry.
 */
class FatalSignalCallbackRegistry {
 public:
  FatalSignalCallbackRegistry();

  void add(SignalCallback func);
  void markInstalled();
  void run();

 private:
  std::atomic<bool> installed_;
  std::mutex mutex_;
  std::vector<SignalCallback> handlers_;
};

FatalSignalCallbackRegistry::FatalSignalCallbackRegistry()
  : installed_(false) {
}

void FatalSignalCallbackRegistry::add(SignalCallback func) {
  std::lock_guard<std::mutex> lock(mutex_);
  CHECK(!installed_)
    << "FatalSignalCallbackRegistry::add may not be used "
       "after installing the signal handlers.";
  handlers_.push_back(func);
}

void FatalSignalCallbackRegistry::markInstalled() {
  std::lock_guard<std::mutex> lock(mutex_);
  CHECK(!installed_.exchange(true))
    << "FatalSignalCallbackRegistry::markInstalled must be called "
    << "at most once";
}

void FatalSignalCallbackRegistry::run() {
  if (!installed_) {
    return;
  }

  for (auto& fn : handlers_) {
    fn();
  }
}

// Leak it so we don't have to worry about destruction order
FatalSignalCallbackRegistry* gFatalSignalCallbackRegistry =
  new FatalSignalCallbackRegistry;

struct {
  int number;
  const char* name;
  struct sigaction oldAction;
} kFatalSignals[] = {
  { SIGSEGV, "SIGSEGV" },
  { SIGILL,  "SIGILL"  },
  { SIGFPE,  "SIGFPE"  },
  { SIGABRT, "SIGABRT" },
  { SIGBUS,  "SIGBUS"  },
  { SIGTERM, "SIGTERM" },
  { 0,       nullptr   }
};

void callPreviousSignalHandler(int signum) {
  // Restore disposition to old disposition, then kill ourselves with the same
  // signal. The signal will be blocked until we return from our handler,
  // then it will invoke the default handler and abort.
  for (auto p = kFatalSignals; p->name; ++p) {
    if (p->number == signum) {
      sigaction(signum, &p->oldAction, nullptr);
      raise(signum);
      return;
    }
  }

  // Not one of the signals we know about. Oh well. Reset to default.
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;
  sigaction(signum, &sa, nullptr);
  raise(signum);
}

constexpr size_t kDefaultCapacity = 500;

// Note: not thread-safe, but that's okay, as we only let one thread
// in our signal handler at a time.
//
// Leak it so we don't have to worry about destruction order
auto gSignalSafeElfCache = new SignalSafeElfCache(kDefaultCapacity);

// Buffered writer (using a fixed-size buffer). We try to write only once
// to prevent interleaving with messages written from other threads.
//
// Leak it so we don't have to worry about destruction order.
auto gPrinter = new FDSymbolizePrinter(STDERR_FILENO,
                                       SymbolizePrinter::COLOR_IF_TTY,
                                       size_t(64) << 10);  // 64KiB

// Flush gPrinter, also fsync, in case we're about to crash again...
void flush() {
  gPrinter->flush();
  fsyncNoInt(STDERR_FILENO);
}

void printDec(uint64_t val) {
  char buf[20];
  uint32_t n = uint64ToBufferUnsafe(val, buf);
  gPrinter->print(StringPiece(buf, n));
}

const char kHexChars[] = "0123456789abcdef";
void printHex(uint64_t val) {
  // TODO(tudorb): Add this to folly/Conv.h
  char buf[2 + 2 * sizeof(uint64_t)];  // "0x" prefix, 2 digits for each byte

  char* end = buf + sizeof(buf);
  char* p = end;
  do {
    *--p = kHexChars[val & 0x0f];
    val >>= 4;
  } while (val != 0);
  *--p = 'x';
  *--p = '0';

  gPrinter->print(StringPiece(p, end));
}

void print(StringPiece sp) {
  gPrinter->print(sp);
}

void dumpTimeInfo() {
  SCOPE_EXIT { flush(); };
  time_t now = time(nullptr);
  print("*** Aborted at ");
  printDec(now);
  print(" (Unix time, try 'date -d @");
  printDec(now);
  print("') ***\n");
}

const char* sigill_reason(int si_code) {
  switch (si_code) {
    case ILL_ILLOPC:
      return "illegal opcode";
    case ILL_ILLOPN:
      return "illegal operand";
    case ILL_ILLADR:
      return "illegal addressing mode";
    case ILL_ILLTRP:
      return "illegal trap";
    case ILL_PRVOPC:
      return "privileged opcode";
    case ILL_PRVREG:
      return "privileged register";
    case ILL_COPROC:
      return "coprocessor error";
    case ILL_BADSTK:
      return "internal stack error";

    default:
      return nullptr;
  }
}

const char* sigfpe_reason(int si_code) {
  switch (si_code) {
    case FPE_INTDIV:
      return "integer divide by zero";
    case FPE_INTOVF:
      return "integer overflow";
    case FPE_FLTDIV:
      return "floating-point divide by zero";
    case FPE_FLTOVF:
      return "floating-point overflow";
    case FPE_FLTUND:
      return "floating-point underflow";
    case FPE_FLTRES:
      return "floating-point inexact result";
    case FPE_FLTINV:
      return "floating-point invalid operation";
    case FPE_FLTSUB:
      return "subscript out of range";

    default:
      return nullptr;
  }
}

const char* sigsegv_reason(int si_code) {
  switch (si_code) {
    case SEGV_MAPERR:
      return "address not mapped to object";
    case SEGV_ACCERR:
      return "invalid permissions for mapped object";

    default:
      return nullptr;
  }
}

const char* sigbus_reason(int si_code) {
  switch (si_code) {
    case BUS_ADRALN:
      return "invalid address alignment";
    case BUS_ADRERR:
      return "nonexistent physical address";
    case BUS_OBJERR:
      return "object-specific hardware error";

    // MCEERR_AR and MCEERR_AO: in sigaction(2) but not in headers.

    default:
      return nullptr;
  }
}

const char* sigtrap_reason(int si_code) {
  switch (si_code) {
    case TRAP_BRKPT:
      return "process breakpoint";
    case TRAP_TRACE:
      return "process trace trap";

    // TRAP_BRANCH and TRAP_HWBKPT: in sigaction(2) but not in headers.

    default:
      return nullptr;
  }
}

const char* sigchld_reason(int si_code) {
  switch (si_code) {
    case CLD_EXITED:
      return "child has exited";
    case CLD_KILLED:
      return "child was killed";
    case CLD_DUMPED:
      return "child terminated abnormally";
    case CLD_TRAPPED:
      return "traced child has trapped";
    case CLD_STOPPED:
      return "child has stopped";
    case CLD_CONTINUED:
      return "stopped child has continued";

    default:
      return nullptr;
  }
}

const char* sigio_reason(int si_code) {
  switch (si_code) {
    case POLL_IN:
      return "data input available";
    case POLL_OUT:
      return "output buffers available";
    case POLL_MSG:
      return "input message available";
    case POLL_ERR:
      return "I/O error";
    case POLL_PRI:
      return "high priority input available";
    case POLL_HUP:
      return "device disconnected";

    default:
      return nullptr;
  }
}

const char* signal_reason(int signum, int si_code) {
  switch (signum) {
    case SIGILL:
      return sigill_reason(si_code);
    case SIGFPE:
      return sigfpe_reason(si_code);
    case SIGSEGV:
      return sigsegv_reason(si_code);
    case SIGBUS:
      return sigbus_reason(si_code);
    case SIGTRAP:
      return sigtrap_reason(si_code);
    case SIGCHLD:
      return sigchld_reason(si_code);
    case SIGIO:
      return sigio_reason(si_code); // aka SIGPOLL

    default:
      return nullptr;
  }
}

void dumpSignalInfo(int signum, siginfo_t* siginfo) {
  SCOPE_EXIT { flush(); };
  // Get the signal name, if possible.
  const char* name = nullptr;
  for (auto p = kFatalSignals; p->name; ++p) {
    if (p->number == signum) {
      name = p->name;
      break;
    }
  }

  print("*** Signal ");
  printDec(signum);
  if (name) {
    print(" (");
    print(name);
    print(")");
  }

  print(" (");
  printHex(reinterpret_cast<uint64_t>(siginfo->si_addr));
  print(") received by PID ");
  printDec(getpid());
  print(" (pthread TID ");
  printHex((uint64_t)pthread_self());
  print(") (linux TID ");
  printDec(syscall(__NR_gettid));

  // Kernel-sourced signals don't give us useful info for pid/uid.
  if (siginfo->si_code != SI_KERNEL) {
    print(") (maybe from PID ");
    printDec(siginfo->si_pid);
    print(", UID ");
    printDec(siginfo->si_uid);
  }

  auto reason = signal_reason(signum, siginfo->si_code);

  if (reason != nullptr) {
    print(") (code: ");
    print(reason);
  }

  print("), stack trace: ***\n");
}

FOLLY_NOINLINE void dumpStackTrace(bool symbolize);

void dumpStackTrace(bool symbolize) {
  SCOPE_EXIT { flush(); };
  // Get and symbolize stack trace
  constexpr size_t kMaxStackTraceDepth = 100;
  FrameArray<kMaxStackTraceDepth> addresses;

  // Skip the getStackTrace frame
  if (!getStackTraceSafe(addresses)) {
    print("(error retrieving stack trace)\n");
  } else if (symbolize) {
    Symbolizer symbolizer(gSignalSafeElfCache);
    symbolizer.symbolize(addresses);

    // Skip the top 2 frames:
    // getStackTraceSafe
    // dumpStackTrace (here)
    //
    // Leaving signalHandler on the stack for clarity, I think.
    gPrinter->println(addresses, 2);
  } else {
    print("(safe mode, symbolizer not available)\n");
    AddressFormatter formatter;
    for (size_t i = 0; i < addresses.frameCount; ++i) {
      print(formatter.format(addresses.addresses[i]));
      print("\n");
    }
  }
}

// On Linux, pthread_t is a pointer, so 0 is an invalid value, which we
// take to indicate "no thread in the signal handler".
//
// POSIX defines PTHREAD_NULL for this purpose, but that's not available.
constexpr pthread_t kInvalidThreadId = 0;

std::atomic<pthread_t> gSignalThread(kInvalidThreadId);
std::atomic<bool> gInRecursiveSignalHandler(false);

// Here be dragons.
void innerSignalHandler(int signum, siginfo_t* info, void* /* uctx */) {
  // First, let's only let one thread in here at a time.
  pthread_t myId = pthread_self();

  pthread_t prevSignalThread = kInvalidThreadId;
  while (!gSignalThread.compare_exchange_strong(prevSignalThread, myId)) {
    if (pthread_equal(prevSignalThread, myId)) {
      // First time here. Try to dump the stack trace without symbolization.
      // If we still fail, well, we're mightily screwed, so we do nothing the
      // next time around.
      if (!gInRecursiveSignalHandler.exchange(true)) {
        print("Entered fatal signal handler recursively. We're in trouble.\n");
        dumpStackTrace(false);  // no symbolization
      }
      return;
    }

    // Wait a while, try again.
    timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 100L * 1000 * 1000;  // 100ms
    nanosleep(&ts, nullptr);

    prevSignalThread = kInvalidThreadId;
  }

  dumpTimeInfo();
  dumpSignalInfo(signum, info);
  dumpStackTrace(true);  // with symbolization

  // Run user callbacks
  gFatalSignalCallbackRegistry->run();
}

void signalHandler(int signum, siginfo_t* info, void* uctx) {
  SCOPE_EXIT { flush(); };
  innerSignalHandler(signum, info, uctx);

  gSignalThread = kInvalidThreadId;
  // Kill ourselves with the previous handler.
  callPreviousSignalHandler(signum);
}

}  // namespace

void addFatalSignalCallback(SignalCallback cb) {
  gFatalSignalCallbackRegistry->add(cb);
}

void installFatalSignalCallbacks() {
  gFatalSignalCallbackRegistry->markInstalled();
}

namespace {

std::atomic<bool> gAlreadyInstalled;

}  // namespace

void installFatalSignalHandler() {
  if (gAlreadyInstalled.exchange(true)) {
    // Already done.
    return;
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sigemptyset(&sa.sa_mask);
  // By default signal handlers are run on the signaled thread's stack.
  // In case of stack overflow running the SIGSEGV signal handler on
  // the same stack leads to another SIGSEGV and crashes the program.
  // Use SA_ONSTACK, so alternate stack is used (only if configured via
  // sigaltstack).
  sa.sa_flags |= SA_SIGINFO | SA_ONSTACK;
  sa.sa_sigaction = &signalHandler;

  for (auto p = kFatalSignals; p->name; ++p) {
    CHECK_ERR(sigaction(p->number, &sa, &p->oldAction));
  }
}

}}  // namespaces
