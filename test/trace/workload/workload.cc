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
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include "absl/cleanup/cleanup.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
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

void runReadWrite() {
  const std::string path = "read-write.txt";
  auto fd_or = Open(path, O_RDWR | O_CREAT, 0644);
  if (!fd_or.ok()) {
    err(1, "open(O_CREAT): %s", fd_or.error().ToString().c_str());
  }
  auto cleaup = absl::MakeCleanup([path] { unlink(path.c_str()); });

  auto fd = std::move(fd_or.ValueOrDie());

  // Test different flavors of write.
  char msg[] = "hello world";
  if (WriteFd(fd.get(), msg, arraysize(msg)) < 0) {
    err(1, "write");
  }
  if (PwriteFd(fd.get(), msg, arraysize(msg), 10) < 0) {
    err(1, "pwrite");
  }

  struct iovec write_vecs[] = {
      {
          .iov_base = msg,
          .iov_len = arraysize(msg),
      },
      {
          .iov_base = msg,
          .iov_len = arraysize(msg) / 2,
      },
  };
  if (writev(fd.get(), write_vecs, arraysize(write_vecs)) < 0) {
    err(1, "writev");
  }
  if (pwritev(fd.get(), write_vecs, arraysize(write_vecs), 10) < 0) {
    err(1, "pwritev");
  }
  if (pwritev2(fd.get(), write_vecs, arraysize(write_vecs), 10, RWF_HIPRI) <
      0) {
    err(1, "pwritev2");
  }

  // Rewind the file and test different flavors of read.
  if (lseek(fd.get(), 0, SEEK_SET) < 0) {
    err(1, "seek(0)");
  }
  char buf[1024];
  if (ReadFd(fd.get(), buf, arraysize(buf)) < 0) {
    err(1, "read");
  }
  if (PreadFd(fd.get(), buf, arraysize(buf), 20) < 0) {
    err(1, "read");
  }

  // Reuse same buffer, since it's not using the result anyways.
  struct iovec read_vecs[] = {
      {
          .iov_base = buf,
          .iov_len = arraysize(msg),
      },
      {
          .iov_base = buf,
          .iov_len = arraysize(msg) / 2,
      },
  };
  if (readv(fd.get(), read_vecs, arraysize(read_vecs)) < 0) {
    err(1, "writev");
  }
  if (preadv(fd.get(), read_vecs, arraysize(read_vecs), 20) < 0) {
    err(1, "pwritev");
  }
  if (preadv2(fd.get(), read_vecs, arraysize(read_vecs), 20, RWF_HIPRI) < 0) {
    err(1, "pwritev2");
  }
}

}  // namespace testing
}  // namespace gvisor

int main(int argc, char** argv) {
  ::gvisor::testing::runForkExecve();
  ::gvisor::testing::runSocket();
  ::gvisor::testing::runReadWrite();

  return 0;
}
