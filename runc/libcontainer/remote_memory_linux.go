package libcontainer

/*
#cgo LDFLAGS: -llz4
#include <lz4.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static inline void init_uffdio_register(struct uffdio_register *reg, uint64_t start, uint64_t len) {
    memset(reg, 0, sizeof(*reg));
    reg->range.start = start;
    reg->range.len = len;
    reg->mode = UFFDIO_REGISTER_MODE_MISSING;
}


static int uffd_api(int fd, struct uffdio_api *api) {
    return ioctl(fd, UFFDIO_API, api);
}

static int uffd_register(int fd, struct uffdio_register *reg) {
    return ioctl(fd, UFFDIO_REGISTER, reg);
}

static int uffd_copy(int fd, struct uffdio_copy *copy) {
    return ioctl(fd, UFFDIO_COPY, copy);
}
*/
import "C"

import (
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"unsafe"

	"github.com/opencontainers/runc/libcontainer/configs"
	"github.com/sirupsen/logrus"
	"golang.org/x/sys/unix"
)

const (
	pageSize      = 4096
	magicRequest  = 0x52414D51
	magicResponse = 0x52414D53
	cmdFetch      = 1
)

type request_t struct {
	Magic   uint32
	Command uint32
	Address uint64
	Count   uint32
}

type response_t struct {
	Magic          uint32
	Status         uint32
	Address        uint64
	OriginalSize   uint32
	CompressedSize uint32
}

func RunRemoteMemoryClient(config *configs.RemoteMemory) error {
	poolSize := config.Swap
	if poolSize == 0 {
		poolSize = config.RAM
	}
	logrus.Infof("Remote memory client starting: server=%s, total_pool=%d bytes (RAM=%d, SWAP=%d)", config.ServerAddress, poolSize, config.RAM, config.Swap)

	// Open userfaultfd
	fd, _, errSys := unix.Syscall(unix.SYS_USERFAULTFD, unix.O_CLOEXEC, 0, 0)
	if errSys != 0 {
		return fmt.Errorf("userfaultfd syscall failed: %v", errSys)
	}
	uffd := int(fd)
	defer unix.Close(uffd)

	// API handshake
	var api C.struct_uffdio_api
	api.api = C.UFFD_API
	api.features = 0
	if res := C.uffd_api(C.int(uffd), &api); res < 0 {
		return fmt.Errorf("UFFDIO_API failed")
	}

	// Map memory pool (shared memory, so container can access it if we know the address)
	// Actually, for a seamless experience, we should probably use UFFD_FEATURE_EVENT_FORK
	// but that is complex. For now, we map the pool as requested.
	pool, err := unix.Mmap(-1, 0, int(poolSize), unix.PROT_READ|unix.PROT_WRITE, unix.MAP_PRIVATE|unix.MAP_ANONYMOUS)
	if err != nil {
		return fmt.Errorf("mmap failed: %v", err)
	}
	defer unix.Munmap(pool)

	// Register pool with uffd
	var reg C.struct_uffdio_register
	C.init_uffdio_register(&reg, C.uint64_t(uintptr(unsafe.Pointer(&pool[0]))), C.uint64_t(poolSize))
	if res := C.uffd_register(C.int(uffd), &reg); res < 0 {
		return fmt.Errorf("UFFDIO_REGISTER failed")
	}

	logrus.Infof("Remote memory pool registered at %p", &pool[0])

	// Connect to server
	conn, err := net.Dial("tcp", config.ServerAddress)
	if err != nil {
		return fmt.Errorf("failed to connect to memory server: %v", err)
	}
	defer conn.Close()

	// Fault handling loop
	msgSize := unsafe.Sizeof(C.struct_uffd_msg{})
	buf := make([]byte, msgSize)

	pageBuf := make([]byte, pageSize)
	compBuf := make([]byte, pageSize*2)

	for {
		n, err := unix.Read(uffd, buf)
		if err != nil {
			if err == unix.EAGAIN {
				continue
			}
			return fmt.Errorf("read uffd failed: %v", err)
		}
		if n != int(msgSize) {
			continue
		}

		msg := (*C.struct_uffd_msg)(unsafe.Pointer(&buf[0]))
		if msg.event != C.UFFD_EVENT_PAGEFAULT {
			continue
		}

		faultAddr := uint64(*(*C.uint64_t)(unsafe.Pointer(&msg.arg[0])))
		pageAddr := faultAddr & ^uint64(pageSize-1)

		// Request page from server
		req := request_t{
			Magic:   magicRequest,
			Command: cmdFetch,
			Address: pageAddr - uint64(uintptr(unsafe.Pointer(&pool[0]))),
			Count:   1,
		}

		if err := binary.Write(conn, binary.LittleEndian, req); err != nil {
			logrus.Errorf("failed to send request: %v", err)
			continue
		}

		var resp response_t
		if err := binary.Read(conn, binary.LittleEndian, &resp); err != nil {
			logrus.Errorf("failed to read response header: %v", err)
			continue
		}

		if resp.Magic != magicResponse {
			logrus.Errorf("invalid response magic: 0x%x", resp.Magic)
			continue
		}

		var data []byte
		if resp.CompressedSize > 0 {
			if int(resp.CompressedSize) > len(compBuf) {
				compBuf = make([]byte, resp.CompressedSize)
			}
			if _, err := io.ReadFull(conn, compBuf[:resp.CompressedSize]); err != nil {
				logrus.Errorf("failed to read compressed data: %v", err)
				continue
			}
			// Decompress
			res := C.LZ4_decompress_safe(
				(*C.char)(unsafe.Pointer(&compBuf[0])),
				(*C.char)(unsafe.Pointer(&pageBuf[0])),
				C.int(resp.CompressedSize),
				C.int(pageSize),
			)
			if res < 0 {
				logrus.Errorf("LZ4 decompression failed: %d", res)
				continue
			}
			data = pageBuf
		} else {
			if _, err := io.ReadFull(conn, pageBuf); err != nil {
				logrus.Errorf("failed to read page data: %v", err)
				continue
			}
			data = pageBuf
		}

		// Inject page with UFFDIO_COPY
		var copy C.struct_uffdio_copy
		copy.dst = C.__u64(pageAddr)
		copy.src = C.__u64(uintptr(unsafe.Pointer(&data[0])))
		copy.len = C.__u64(pageSize)
		copy.mode = 0
		copy.copy = 0

		if res := C.uffd_copy(C.int(uffd), &copy); res < 0 {
			logrus.Errorf("UFFDIO_COPY failed: %v", res)
		}
	}
}
