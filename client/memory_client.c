#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <fcntl.h>
#include <lz4.h>
#include <errno.h>
#include "protocol.h"

int server_sock;
int uffd;

void *fault_handler(void *arg) {
    struct uffd_msg msg;
    char compressed[LZ4_COMPRESSBOUND(PAGE_SIZE)];
    char decompressed[PAGE_SIZE];

    for (;;) {
        ssize_t nread = read(uffd, &msg, sizeof(msg));
        if (nread == 0) continue;
        if (nread < 0) {
            if (errno == EAGAIN) continue;
            perror("read uffd");
            break;
        }

        if (msg.event == UFFD_EVENT_PAGEFAULT) {
            uint64_t addr = msg.arg.pagefault.address & ~(PAGE_SIZE - 1);
            // printf("Fault at %p\n", (void *)addr);
            
            request_t req = {
                .magic = MAGIC_REQUEST,
                .command = CMD_FETCH,
                .address = addr,
                .count = 4 // Prefetch 4 pages
            };

            if (send(server_sock, &req, sizeof(req), 0) < 0) {
                perror("send request");
                break;
            }

            // The server will send at least the requested page, and up to count additional pages
            for (int i = 0; i < 5; i++) {
                response_t resp;
                if (recv(server_sock, &resp, sizeof(resp), MSG_WAITALL) != sizeof(resp)) break;
                if (resp.magic != MAGIC_RESPONSE) break;

                if (resp.status == 200 && resp.compressed_size > 0) {
                    if (recv(server_sock, compressed, resp.compressed_size, MSG_WAITALL) != resp.compressed_size) break;
                    LZ4_decompress_safe(compressed, decompressed, resp.compressed_size, PAGE_SIZE);

                    struct uffdio_copy copy = {
                        .dst = resp.address,
                        .src = (uint64_t)decompressed,
                        .len = PAGE_SIZE,
                        .mode = 0
                    };

                    if (ioctl(uffd, UFFDIO_COPY, &copy) < 0) {
                        // It's possible the page was already copied by a parallel fault or prefetch
                        if (errno != EEXIST) {
                            perror("UFFDIO_COPY");
                        }
                    }
                } else if (resp.status == 404) {
                    // Page not found, maybe handle by injecting zeros or failing
                    struct uffdio_zeropage zero = {
                        .range = {.start = resp.address, .len = PAGE_SIZE},
                        .mode = 0
                    };
                    ioctl(uffd, UFFDIO_ZEROPAGE, &zero);
                }

                // If this was the original faulting page, we might want to continue to get prefetches
                // but if we got what we came for, we can potentially return to let the thread continue
                // while prefetch continues in background? For simplicity, we just block until done.
            }
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port> <mem_size_mb>\n", argv[0]);
        return 1;
    }

    char *ip = argv[1];
    int port = atoi(argv[2]);
    size_t mem_size = (size_t)atoi(argv[3]) * 1024 * 1024;

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, ip, &addr.sin_addr);

    if (connect(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (uffd == -1) {
        perror("userfaultfd");
        return 1;
    }

    struct uffdio_api api = {.api = UFFD_API, .features = 0};
    if (ioctl(uffd, UFFDIO_API, &api) < 0) {
        perror("UFFDIO_API");
        return 1;
    }

    void *region = mmap(NULL, mem_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    struct uffdio_register reg = {
        .range = {.start = (uint64_t)region, .len = mem_size},
        .mode = UFFDIO_REGISTER_MODE_MISSING
    };
    if (ioctl(uffd, UFFDIO_REGISTER, &reg) < 0) {
        perror("UFFDIO_REGISTER");
        return 1;
    }

    printf("Memory client initialized. Region: %p, Size: %zu MB\n", region, mem_size / 1024 / 1024);

    pthread_t thread;
    pthread_create(&thread, NULL, fault_handler, NULL);

    // Test access
    char *p = (char *)region;
    printf("Accessing remote memory...\n");
    for (size_t i = 0; i < mem_size; i += PAGE_SIZE * 1024) {
        p[i] = 'X';
        printf("Written to offset %zu\n", i);
    }
    printf("Test complete. Press Enter to exit.\n");
    getchar();

    return 0;
}
