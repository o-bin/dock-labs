#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <lz4.h>
#include "protocol.h"

#define PORT 8888
#define DEFAULT_POOL_SIZE (512LL * 1024LL * 1024LL) // 512MB default

void *memory_pool = NULL;
size_t pool_size = DEFAULT_POOL_SIZE;

#include <pthread.h>

void send_page(int client_sock, uint64_t addr, char *data) {
    char compressed[LZ4_COMPRESSBOUND(PAGE_SIZE)];
    int comp_size = LZ4_compress_default(data, compressed, PAGE_SIZE, sizeof(compressed));

    response_t resp = {
        .magic = MAGIC_RESPONSE,
        .status = 200,
        .address = addr,
        .original_size = PAGE_SIZE,
        .compressed_size = (uint32_t)comp_size
    };

    send(client_sock, &resp, sizeof(resp), 0);
    send(client_sock, compressed, comp_size, 0);
}

void *handle_client(void *arg) {
    int client_sock = *(int *)arg;
    free(arg);

    request_t req;
    while (recv(client_sock, &req, sizeof(req), MSG_WAITALL) == sizeof(req)) {
        if (req.magic != MAGIC_REQUEST) continue;

        if (req.command == CMD_FETCH || req.command == CMD_PREFETCH) {
            uint64_t base_addr = req.address;
            uint32_t count = (req.command == CMD_FETCH) ? req.count + 1 : req.count;

            for (uint32_t i = 0; i < count; i++) {
                uint64_t current_addr = base_addr + (i * PAGE_SIZE);
                if (current_addr + PAGE_SIZE > pool_size) {
                    if (i == 0) {
                        response_t resp = {MAGIC_RESPONSE, 404, current_addr, 0, 0};
                        send(client_sock, &resp, sizeof(resp), 0);
                    }
                    break;
                }

                char *data = (char *)memory_pool + current_addr;
                send_page(client_sock, current_addr, data);
            }
        }
    }
    close(client_sock);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc > 1) {
        pool_size = (size_t)atoll(argv[1]) * 1024 * 1024;
    }

    // Allocate pool with 4K alignment (mmap does this by default on Linux)
    memory_pool = mmap(NULL, pool_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory_pool == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    // Initialize with something to see it working
    memset(memory_pool, 0, pool_size);
    printf("Memory pool of %zu MB allocated at %p\n", pool_size / (1024 * 1024), memory_pool);

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_sock, 10) < 0) {
        perror("listen");
        return 1;
    }

    printf("Server listening on port %d...\n", PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            perror("accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        printf("Client connected from %s\n", client_ip); // Added printf statement

        int *p_sock = malloc(sizeof(int));
        *p_sock = client_sock;
        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_client, p_sock) != 0) {
            perror("pthread_create");
            close(client_sock);
            free(p_sock);
        } else {
            pthread_detach(thread);
        }
    }

    return 0;
}
