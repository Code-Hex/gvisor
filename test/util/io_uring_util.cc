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

#include "test/util/io_uring_util.h"

#include <memory>

#include "test/util/temp_path.h"
#include "test/util/test_util.h"

namespace gvisor {
namespace testing {

void output_to_console(char *buf, int len) {
  while (len--) {
    fputc(*buf++, stdout);
  }
}

PosixErrorOr<std::unique_ptr<IOUring>> IOUring::InitIOUring(
    unsigned int entries, IOUringParams &params) {
  PosixErrorOr<FileDescriptor> fd = NewIOUringFD(entries, params);
  if (!fd.ok()) {
    return fd.error();
  }

  return std::make_unique<IOUring>(std::move(fd.ValueOrDie()), entries, params);
}

IOUring::IOUring(FileDescriptor &&fd, unsigned int entries,
                 IOUringParams &params)
    : iouringfd_(std::move(fd)) {
  cring_sz_ = params.cq_off.cqes + params.cq_entries * sizeof(IOUringCqe);
  sring_sz_ = params.sq_off.array + params.sq_entries * sizeof(unsigned);
  sqes_sz_ = params.sq_entries * sizeof(IOUringSqe);

  cq_ptr_ =
      mmap(0, cring_sz_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
           iouringfd_.get(), IORING_OFF_SQ_RING);
  sq_ptr_ =
      mmap(0, sring_sz_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
           iouringfd_.get(), IORING_OFF_SQ_RING);
  sqe_ptr_ = mmap(0, sqes_sz_, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_POPULATE, iouringfd_.get(), IORING_OFF_SQES);

  cqes_ = reinterpret_cast<IOUringCqe *>(reinterpret_cast<char *>(cq_ptr_) +
                                         params.cq_off.cqes);

  cq_head_ptr_ = reinterpret_cast<uint32_t *>(
      reinterpret_cast<char *>(cq_ptr_) + params.cq_off.head);
  cq_tail_ptr_ = reinterpret_cast<uint32_t *>(
      reinterpret_cast<char *>(cq_ptr_) + params.cq_off.tail);
  sq_head_ptr_ = reinterpret_cast<uint32_t *>(
      reinterpret_cast<char *>(sq_ptr_) + params.sq_off.head);
  sq_tail_ptr_ = reinterpret_cast<uint32_t *>(
      reinterpret_cast<char *>(sq_ptr_) + params.sq_off.tail);
  cq_overflow_ptr_ = reinterpret_cast<uint32_t *>(
      reinterpret_cast<char *>(cq_ptr_) + params.cq_off.overflow);
  sq_dropped_ptr_ = reinterpret_cast<uint32_t *>(
      reinterpret_cast<char *>(sq_ptr_) + params.sq_off.dropped);

  sq_mask_ = *(reinterpret_cast<uint32_t *>(reinterpret_cast<char *>(sq_ptr_) +
                                            params.sq_off.ring_mask));
  sq_array_ = reinterpret_cast<unsigned *>(reinterpret_cast<char *>(sq_ptr_) +
                                           params.sq_off.array);
}

IOUring::~IOUring() {
  munmap(cq_ptr_, cring_sz_);
  munmap(sq_ptr_, sring_sz_);
  munmap(sqe_ptr_, sqes_sz_);
}

uint32_t IOUring::load_cq_head() { return io_uring_atomic_read(cq_head_ptr_); }

uint32_t IOUring::load_cq_tail() { return io_uring_atomic_read(cq_tail_ptr_); }

uint32_t IOUring::load_sq_head() { return io_uring_atomic_read(sq_head_ptr_); }

uint32_t IOUring::load_sq_tail() { return io_uring_atomic_read(sq_tail_ptr_); }

uint32_t IOUring::load_cq_overflow() {
  return io_uring_atomic_read(cq_overflow_ptr_);
}

uint32_t IOUring::load_sq_dropped() {
  return io_uring_atomic_read(sq_dropped_ptr_);
}

void IOUring::store_cq_head(uint32_t cq_head_val) {
  io_uring_atomic_write(cq_head_ptr_, cq_head_val);
}

void IOUring::store_sq_tail(uint32_t sq_tail_val) {
  io_uring_atomic_write(sq_tail_ptr_, sq_tail_val);
}

int IOUring::Enter(unsigned int to_submit, unsigned int min_complete,
                   unsigned int flags, sigset_t *sig) {
  return IOUringEnter(iouringfd_.get(), to_submit, min_complete, flags, sig);
}

IOUringCqe *IOUring::get_cqes() { return cqes_; }

IOUringSqe *IOUring::get_sqes() {
  return reinterpret_cast<IOUringSqe *>(sqe_ptr_);
}

uint32_t IOUring::get_sq_mask() { return sq_mask_; }

unsigned *IOUring::get_sq_array() { return sq_array_; }

IOUringTestFile::IOUringTestFile(std::string text) : file_rw_offset_{0} {
  file_name_ = NewTempAbsPath();
  fd_ = open(file_name_.c_str(), O_CREAT, 0644);
  if (fd_ < 0) {
    fd_ = -1;

    return;
  }
  close(fd_);
  fd_ = open(file_name_.c_str(), O_RDWR);

  Write(std::move(text));
}

IOUringTestFile::~IOUringTestFile() {
  close(fd_);
  unlink(file_name_.c_str());
  if (file_info_ != nullptr) {
    free(file_info_);
  }
}

// TODO: return should be value or posix error
void IOUringTestFile::Write(std::string &&text) {
  PwriteFd(fd_, text.c_str(), text.size(), file_rw_offset_);
  file_rw_offset_ += text.size();
  UpdateTestFileInfo();
}

void IOUringTestFile::UpdateTestFileInfo() {
  off_t bytes_remaining = FileSize();
  unsigned current_block = 0;

  num_blocks_ = (int)bytes_remaining / BLOCK_SZ;
  if (bytes_remaining % BLOCK_SZ) {
    num_blocks_++;
  }

  file_info_ = (struct file_info *)malloc(sizeof(off_t) +
                                          sizeof(struct iovec) * num_blocks_);
  file_info_->file_sz = FileSize();

  while (bytes_remaining) {
    off_t bytes_to_read = bytes_remaining;
    if (bytes_to_read > BLOCK_SZ) bytes_to_read = BLOCK_SZ;

    file_info_->iovecs[current_block].iov_len = bytes_to_read;

    void *buf;
    if (posix_memalign(&buf, BLOCK_SZ, BLOCK_SZ)) {
      perror("posix_memalign");
      return;
    }
    file_info_->iovecs[current_block].iov_base = buf;

    current_block++;
    bytes_remaining -= bytes_to_read;
  }
}

// TODO: return should be value or posix error
off_t IOUringTestFile::FileSize() const {
  struct stat st;

  if (fstat(fd_, &st) < 0) {
    return -1;
  }

  if (S_ISBLK(st.st_mode)) {
    uint64_t bytes;
    if (ioctl(fd_, BLKGETSIZE64, &bytes) != 0) {
      return -1;
    }

    return bytes;
  }
  if (S_ISREG(st.st_mode)) {
    return st.st_size;
  }

  return -1;
}

TestFileInfo *IOUringTestFile::FileInfo() {
  if (file_info_ == nullptr) {
    UpdateTestFileInfo();
  }

  return file_info_;
}

}  // namespace testing
}  // namespace gvisor
