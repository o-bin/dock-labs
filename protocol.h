#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define PAGE_SIZE 4096
#define MAGIC_REQUEST 0x52414D51 // "RAMQ"
#define MAGIC_RESPONSE 0x52414D53 // "RAMS"

typedef enum {
    CMD_FETCH = 1,      // Request a single page
    CMD_PREFETCH = 2,   // Request N pages starting from address
    CMD_EVICT = 3       // Signal that a page can be freed (optional/placeholder)
} command_t;

typedef struct {
    uint32_t magic;
    uint32_t command;
    uint64_t address;
    uint32_t count; // Number of pages for prefetch
} __attribute__((packed)) request_t;

typedef struct {
    uint32_t magic;
    uint32_t status;
    uint64_t address;
    uint32_t original_size;
    uint32_t compressed_size; // 0 if not compressed
} __attribute__((packed)) response_t;

#endif // PROTOCOL_H
