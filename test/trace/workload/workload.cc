// Copyright 2022 The gVisor Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <err.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <csignal>
#include <iostream>
#include <ostream>

#include "absl/cleanup/cleanup.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "test/util/eventfd_util.h"
#include "test/util/file_descriptor.h"
#include "test/util/multiprocess_util.h"
#include "test/util/posix_error.h"
#include "test/util/test_util.h"

namespace gvisor {
namespace testing {

void runForkExecve() {
  auto root_or_error = Open("/", O_RDONLY, 0);
  auto& root = root_or_error.ValueOrDie();

  pid_t child;
  int execve_errno;
  ExecveArray argv = {"/bin/true"};
  ExecveArray envv = {"TEST=123"};
  auto kill_or_error = ForkAndExecveat(root.get(), "/bin/true", argv, envv, 0,
                                       nullptr, &child, &execve_errno);
  ASSERT_EQ(0, execve_errno);
  // Don't kill child, just wait for gracefully exit.
  kill_or_error.ValueOrDie().Release();
  RetryEINTR(waitpid)(child, nullptr, 0);
}

// Creates a simple UDS in the abstract namespace and send one byte from the
// client to the server.
void runSocket() {
  auto path = absl::StrCat(std::string("\0", 1), "trace_test.", getpid(),
                           absl::GetCurrentTimeNanos());

  struct sockaddr_un addr;
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path.c_str(), path.size() + 1);

  int parent_sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (parent_sock < 0) {
    err(1, "socket");
  }
  auto sock_closer = absl::MakeCleanup([parent_sock] { close(parent_sock); });

  if (bind(parent_sock, reinterpret_cast<struct sockaddr*>(&addr),
           sizeof(addr))) {
    err(1, "bind");
  }
  if (listen(parent_sock, 5) < 0) {
    err(1, "listen");
  }

  pid_t pid = fork();
  if (pid < 0) {
    // Fork error.
    err(1, "fork");

  } else if (pid == 0) {
    // Child.
    close(parent_sock);  // ensure it's not mistakely used in child.

    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) {
      err(1, "socket");
    }
    auto server_closer = absl::MakeCleanup([server] { close(server); });

    if (connect(server, reinterpret_cast<struct sockaddr*>(&addr),
                sizeof(addr)) < 0) {
      err(1, "connect");
    }

    char buf = 'A';
    int bytes = write(server, &buf, sizeof(buf));
    if (bytes != 1) {
      err(1, "write: %d", bytes);
    }
    exit(0);

  } else {
    // Parent.
    int client = RetryEINTR(accept)(parent_sock, nullptr, nullptr);
    if (client < 0) {
      err(1, "accept");
    }
    auto client_closer = absl::MakeCleanup([client] { close(client); });

    char buf;
    int bytes = read(client, &buf, sizeof(buf));
    if (bytes != 1) {
      err(1, "read: %d", bytes);
    }

    // Wait to reap the child.
    RetryEINTR(waitpid)(pid, nullptr, 0);
  }
}

void runChdir() {
  const auto pathname = "trace_test.abc";
  static constexpr mode_t kDefaultDirMode = 0755;
  int path_or_error = mkdir(pathname, kDefaultDirMode);
  if (path_or_error != 0) {
    err(1, "mkdir");
  }
  int res = chdir(pathname);
  if (res != 0) {
    err(1, "chdir");
  }
  rmdir(pathname);
}

void runFchdir() {
  const auto pathname = "trace_test.abc";
  static constexpr mode_t kDefaultDirMode = 0755;
  int path_or_error = mkdir(pathname, kDefaultDirMode);
  if (path_or_error != 0) {
    err(1, "mkdir");
  }
  int fd = open(pathname, O_DIRECTORY | O_RDONLY);
  int res = fchdir(fd);
  if (res != 0) {
    err(1, "fchdir");
  }
  rmdir(pathname);
  close(fd);
}

void runSetgid() {
  auto get = setgid(0);
  if (get != 0) {
    err(1, "setgid");
  }
}

void runSetuid() {
  auto get = setuid(0);
  if (get != 0) {
    err(1, "setuid");
  }
}

void runSetsid() {
  auto get = setsid();
  // Operation is not permitted so we get an error.
  if (get != -1) {
    err(1, "setsid");
  }
}

void runSetresuid() {
  auto get = setresuid(0, 0, 0);
  if (get != 0) {
    err(1, "setresuid");
  }
}

void runSetresgid() {
  auto get = setresgid(0, 0, 0);
  if (get != 0) {
    err(1, "setresgid");
  }
}

void runChroot() {
  const auto pathname = "trace_test.abc";
  static constexpr mode_t kDefaultDirMode = 0755;
  int path_or_error = mkdir(pathname, kDefaultDirMode);
  if (path_or_error != 0) {
    err(1, "mkdir");
  }
  if (chroot(pathname)) {
    err(1, "chroot");
  }
  rmdir(pathname);
}
void runDup() {
  const auto pathname = "trace_test.abc";
  static constexpr mode_t kDefaultDirMode = 0755;
  int path_or_error = mkdir(pathname, kDefaultDirMode);
  if (path_or_error != 0) {
    err(1, "mkdir");
  }
  int fd = open(pathname, O_DIRECTORY | O_RDONLY);
  int res = dup(fd);
  if (res < 0) {
    err(1, "dup");
  }
  rmdir(pathname);
}
void runDup2() {
  const auto pathname = "trace_test.abc";
  static constexpr mode_t kDefaultDirMode = 0755;
  int path_or_error = mkdir(pathname, kDefaultDirMode);
  if (path_or_error != 0) {
    err(1, "mkdir");
  }
  int oldfd = open(pathname, O_DIRECTORY | O_RDONLY);
  int newfd = open(pathname, O_DIRECTORY | O_RDONLY);
  int res = dup2(oldfd, newfd);
  if (res != newfd) {
    err(1, "dup2");
  }
  rmdir(pathname);
}
void runDup3() {
  const auto pathname = "trace_test.abc";
  static constexpr mode_t kDefaultDirMode = 0755;
  int path_or_error = mkdir(pathname, kDefaultDirMode);
  if (path_or_error != 0) {
    err(1, "mkdir");
  }
  int oldfd = open(pathname, O_DIRECTORY | O_RDONLY);
  int newfd = open(pathname, O_DIRECTORY | O_RDONLY);
  int res = dup3(oldfd, newfd, O_CLOEXEC);
  if (res != newfd) {
    err(1, "dup3");
  }
  rmdir(pathname);
}

void runPrlimit64() {
  struct rlimit setlim;
  setlim.rlim_cur = 0;
  setlim.rlim_max = RLIM_INFINITY;
  int res = prlimit(0, RLIMIT_DATA, &setlim, nullptr);
  if (res != 0) {
    err(1, "prlimit64");
  }
}

void runEventfd() {
  int res = eventfd(0, EFD_NONBLOCK);
  if (res < 0) {
    err(1, "eventfd");
  }
}

void runEventfd2() {
  int res = Eventdfd2Setup(0, EFD_NONBLOCK);
  if (res < 0) {
    err(1, "eventfd2");
  }
}

void runBind() {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  int res = bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (res < 0) {
    err(1, "bind");
  }
  auto server_closer = absl::MakeCleanup([fd] { close(fd); });
}

void runAccept() {
  struct sockaddr_un addr;
  addr.sun_family = AF_UNIX;

  int server = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server < 0) {
    err(1, "socket");
  }

  int res = bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (res < 0) {
    err(1, "bind");
  }

  if (listen(server, 5) < 0) {
    err(1, "listen");
  }

  int client = socket(AF_UNIX, SOCK_STREAM, 0);
  if (client < 0) {
    err(1, "socket");
  }

  if (connect(client, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) <
      0) {
    err(1, "connect");
  }

  res = RetryEINTR(accept)(server, nullptr, nullptr);
  if (res < 0) {
    err(1, "accept");
  }

  auto server_closer = absl::MakeCleanup([server] { close(server); });
  auto client_closer = absl::MakeCleanup([client] { close(client); });
}

}  // namespace testing
}  // namespace gvisor

int main(int argc, char** argv) {
  ::gvisor::testing::runForkExecve();
  ::gvisor::testing::runSocket();
  ::gvisor::testing::runChdir();
  ::gvisor::testing::runFchdir();
  ::gvisor::testing::runSetgid();
  ::gvisor::testing::runSetuid();
  ::gvisor::testing::runSetsid();
  ::gvisor::testing::runSetresuid();
  ::gvisor::testing::runSetresgid();
  ::gvisor::testing::runChroot();
  ::gvisor::testing::runDup();
  ::gvisor::testing::runDup2();
  ::gvisor::testing::runDup3();
  ::gvisor::testing::runPrlimit64();
  ::gvisor::testing::runEventfd();
  ::gvisor::testing::runEventfd2();
  ::gvisor::testing::runBind();
  ::gvisor::testing::runAccept();

  return 0;
}
