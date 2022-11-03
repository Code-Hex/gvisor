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

package cmd

import (
	"syscall"
	"unsafe"

	"golang.org/x/sys/unix"
	"gvisor.dev/gvisor/pkg/abi/linux"
	"gvisor.dev/gvisor/runsc/cmd/util"
)

//go:linkname beforeFork syscall.runtime_BeforeFork
func beforeFork()

//go:linkname afterFork syscall.runtime_AfterFork
func afterFork()

//go:linkname afterForkInChild syscall.runtime_AfterForkInChild
func afterForkInChild()

//go:nosplit
func procUmounter(sks [2]int, procPathPtr *byte) unix.Errno {
	if _, _, errno := unix.RawSyscall(unix.SYS_CLOSE, uintptr(sks[1]), 0, 0); errno != 0 {
		return errno
	}
	// Wait for when the parent closes its socket.
	c := uint8(0)
	if _, _, errno := unix.RawSyscall(
		unix.SYS_READ,
		uintptr(sks[0]),
		uintptr(unsafe.Pointer(&c)), 1); errno != 0 {
		return errno
	}
	if _, _, errno := unix.RawSyscall(
		unix.SYS_UMOUNT2,
		uintptr(unsafe.Pointer(procPathPtr)),
		uintptr(linux.MNT_DETACH), 0); errno != 0 {
		return errno
	}
	if _, _, errno := unix.RawSyscall(unix.SYS_EXIT_GROUP, 0, 0, 0); errno != 0 {
		return errno
	}
	return 0
}

// forkProcUmounter forks a child process that umounts /proc when the sks[1]
// socket is closed.
func forkProcUmounter(sks [2]int) {
	procPathPtr := syscall.StringBytePtr("/proc")
	beforeFork()
	pid, _, errno := unix.RawSyscall6(unix.SYS_CLONE, uintptr(unix.SIGCHLD), 0, 0, 0, 0, 0)
	if errno != 0 {
		afterFork()
		util.Fatalf("error forking a process: %v", errno)
	}

	if pid == 0 {
		afterForkInChild()
		procUmounter(sks, procPathPtr)
		unix.RawSyscall(unix.SYS_EXIT_GROUP, 1, 0, 0)
	}
	afterFork()
	unix.RawSyscall(unix.SYS_CLOSE, uintptr(sks[0]), 0, 0)
}
