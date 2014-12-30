
#include <assert.h>
#include <time.h>
#include <systemd/sd-bus-vtable.h>
#include "list.h"
#include "util.h"
#include "hashmap.h"

#define POOL_START (1ULL<<31)
#define POOL_END UINT32_MAX
#define CHUNK_MAX_EXP 28
//#define POOL_START 0
//#define POOL_END 2047
//#define CHUNK_MAX_EXP 8

#define CHUNK_MAX (1ULL << (CHUNK_MAX_EXP - 1))
#define INIT_CHUNK_COUNT ((POOL_END - POOL_START + 1) / CHUNK_MAX)

typedef struct Chunk Chunk;

struct Chunk {
        uint64_t start;
        uint32_t size;
        bool allocated;
        Chunk *parent;
        Chunk *children;
        LIST_FIELDS(Chunk, freelist);
};

typedef struct Slice {
        LIST_HEAD(Chunk) list;
} Slice;

Slice pool[CHUNK_MAX_EXP];
Chunk *root;

uint32_t bitsize(uint64_t in) {
        assert(in > 0);
        if (in == 1)
                return 1;
        return (uint32_t) (sizeof(long) * __CHAR_BIT__ - __builtin_clzl(in-1))+1;
}

int chunk_split(Chunk *c) {
        printf("  splitting chunk: start: %llu size: %u (%llu)\n", c->start, c->size, 1UL << (c->size-1));

        c->children = new0(Chunk, 2);
        c->children[0].start = c->start;
        c->children[0].size = c->size -1;
        c->children[0].parent = c;

        c->children[1].start = c->start + (1ull<<(c->size - 2));
        c->children[1].size = c->size -1;
        c->children[1].parent = c;

        return 0;
}

Chunk *chunk_get(uint32_t size) {
        Slice *slice;
        Chunk *chunk;

        if (size > CHUNK_MAX_EXP)
                return NULL;

        slice = &(pool[size-1]);
        chunk = LIST_STEAL_FIRST(freelist, slice->list);
        if (!chunk) {
                Chunk *parent, *children;
                parent = chunk_get(size + 1);
                if (!parent)
                        return NULL;

                assert(parent->size == size + 1);

                chunk_split(parent);
                LIST_PREPEND(freelist, slice->list, &(parent->children[1]));
                chunk = &(parent->children[0]);
        }
        chunk->allocated = true;

        return chunk;
}

Chunk *alloc_chunk(uint64_t size) {
        uint32_t bs;
        Chunk *c;

        bs = bitsize(size);

        c = chunk_get(bs);
        printf(" allocated chunk : start: %llu size: %u (%llu) requested: %u (%llu)\n", c->start, c->size, 1ULL << (c->size-1), bs, size);
        return c;
}

Chunk *free_chunk(Chunk *c) {

        printf(" freeing chunk : start: %llu size: %u (%llu)\n", c->start, c->size, 1ULL << (c->size-1));

        c->allocated = false;

        if (c->parent && c->parent->children[0].allocated == false && c->parent->children[1].allocated == false) {
                Chunk *p = c->parent;

                printf("  mergeing chunk : start: %llu size: %u (%llu)\n", p->start, p->size, 1ULL << (p->size-1));
                LIST_REMOVE(freelist, pool[c->size-1].list, &(p->children[0]));
                LIST_REMOVE(freelist, pool[c->size-1].list, &(p->children[1]));
                free(c->parent->children);
                c->parent->children = NULL;
                free_chunk(p);
        } else {
                LIST_PREPEND(freelist, pool[c->size-1].list, c);
        }

        return NULL;
}

int populate_pool() {
        Slice *slice;
        int i;

        root = new0(Chunk, INIT_CHUNK_COUNT+1);
        slice = &(pool[CHUNK_MAX_EXP-1]);

        for (i = 0; i < INIT_CHUNK_COUNT; i++) {
                root[i].start = POOL_START + (i * CHUNK_MAX);
                root[i].size = CHUNK_MAX_EXP;
                printf("initial chunk %d, start: %llu size: %u (%llu)\n", i+1, root[i].start, root[i].size, 1ULL << (root[i].size)-1);
                LIST_APPEND(freelist, slice->list, &(root[i]));
        }
        return i;
}

typedef struct Lease Lease;
struct Lease {
        Chunk *chunk;
        char *id;
        char *alias;
        uint32_t persistent;
};

Hashmap *leasemap;
Hashmap *aliasmap;

int bus_lease_release(sd_bus *bus, sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
        int r;
        Lease *lease = userdata;

        printf("releaseing: %s\n", lease->id);
        
        hashmap_remove(leasemap, lease->id);

        free_chunk(lease->chunk);
        free(lease->id);
        free(lease->alias);
        free(lease);


        r = sd_bus_reply_method_return(m, "");
        if (r < 0) {
                log_error("Failed to send reply: %s", strerror(-r));
                return r;
        }


        return 1;
}
int bus_lease_get_start(sd_bus *bus, const char *path, const char *interface, const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error) {
        Lease *lease = userdata;

        sd_bus_message_append(reply, "t", lease->chunk->start);
        return 1;
}
int bus_lease_get_end(sd_bus *bus, const char *path, const char *interface, const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error) {
        Lease *lease = userdata;

        sd_bus_message_append(reply, "t", lease->chunk->start + (1ULL << (lease->chunk->size -1)) - 1);
        return 1;
}
int bus_lease_get_size(sd_bus *bus, const char *path, const char *interface, const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error) {
        Lease *lease = userdata;

        sd_bus_message_append(reply, "t", 1ULL << (lease->chunk->size -1));
        return 1;
}

int bus_lease_alloc(sd_bus *bus, sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
        int r;
        uint64_t size;
        uint32_t persistent;
        Lease *lease;
        char path[] = "/be/enospc/uidallocd/leases/xx_xxxxxxxxxxxxxxxx";
        char *id = &(path[28]);
        char *alias = NULL;

        r = sd_bus_message_read(m, "stb", &alias, &size, &persistent);
        if (r < 0) {
                log_error("Failed to read request: %s", strerror(-r));
                return r;
        }
        
        lease = new0(Lease, 1);
        lease->chunk = alloc_chunk(size);

        snprintf(id, 20,"%02x_%016lx", lease->chunk->size, lease->chunk->start);
        lease->id = strdup(id);
        hashmap_put(leasemap, lease->id, lease);

        if (strlen(alias) > 0) {
                int r;
                lease->alias = strdup(alias);

                r = hashmap_put(aliasmap, lease->alias, lease);
                if (r < 0) {
                        sd_bus_reply_method_errno(m, -r, NULL);
                        return 1;
                }
        }

        r = sd_bus_reply_method_return(m, "ott", path, lease->chunk->start, 1ULL << (lease->chunk->size -1));
        if (r < 0) {
                log_error("Failed to send reply: %s", strerror(-r));
                return r;
        }

        return 1;
}

static const sd_bus_vtable lease_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("Release", "", "", bus_lease_release, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_PROPERTY("Start", "t", bus_lease_get_start, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("End", "t", bus_lease_get_end, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Size", "t", bus_lease_get_size, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ID", "s", NULL, offsetof(Lease, id), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Alias", "s", NULL, offsetof(Lease, alias), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_VTABLE_END,
};

static const sd_bus_vtable main_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("AllocUids", "stb", "ott", bus_lease_alloc, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_VTABLE_END,
};

int lease_object_find(sd_bus *bus, const char *path, const char *interface, void *userdata, void **found, sd_bus_error *error) {
        Lease *lease = NULL;
        char *id;
        char *alias;

        id = startswith(path, "/be/enospc/uidallocd/leases/");
        alias = startswith(path, "/be/enospc/uidallocd/aliases/");
        if (id)
                lease = hashmap_get(leasemap, id);
        if (alias)
                lease = hashmap_get(aliasmap, alias);

        if (!lease)
                return 0;

        *found = lease;
        return 1;
}

int main() {
        int r;
        sd_bus *bus = NULL;
        sd_event *event = NULL;

        r = populate_pool();
        if (r < 0)
                goto end;
        leasemap = hashmap_new(&string_hash_ops);
        aliasmap = hashmap_new(&string_hash_ops);


        r = sd_bus_default_user(&bus);
        if (r < 0) {
                log_error("Failed to connect to the bus: %s", strerror(-r));
                goto end;
        }
        r = sd_event_default(&event);
        if (r < 0) {
                log_error("Failed to open event loop: %s", strerror(-r));
                goto end;
        }

        r = sd_bus_add_object_vtable(bus, NULL, "/be/enospc/uidallocd", "be.enospc.uidallocd.Manager", main_vtable, NULL);
        if (r < 0) {
                log_error("Failed to register object: %s", strerror(-r));
                goto end;
        }

        r = sd_bus_add_fallback_vtable(bus, NULL, "/be/enospc/uidallocd/leases", "be.enospc.uidallocd.Lease", lease_vtable, lease_object_find, NULL);
        if (r < 0) {
                log_error("Failed to add lease object vtable: %s", strerror(-r));
                return r;
        }

        r = sd_bus_add_fallback_vtable(bus, NULL, "/be/enospc/uidallocd/aliases", "be.enospc.uidallocd.Lease", lease_vtable, lease_object_find, NULL);
        if (r < 0) {
                log_error("Failed to add lease object vtable: %s", strerror(-r));
                return r;
        }

        r = sd_bus_request_name(bus, "be.enospc.uidallocd", 0);
        if (r < 0) {
                log_error("Failed to register name: %s", strerror(-r));
                goto end;
        }

        r = sd_bus_attach_event(bus, event, 0);
        if (r < 0) {
                log_error("Failed to attach bus to event loop: %s", strerror(-r));
                goto end;
        }

        r = sd_event_loop(event);

end:
        if (r < 0)
                return EXIT_FAILURE;
        return EXIT_SUCCESS;
}

void random_bytes(void *p, size_t n) {
        static bool srand_called = false;
        uint8_t *q;
        int r;

        if (!srand_called) {
                struct timespec ts;
                unsigned x = 0;

                clock_gettime(CLOCK_REALTIME, &ts);
                x ^= (unsigned) ts.tv_sec;
                x ^= (unsigned) ts.tv_nsec;

                srand(x);
                srand_called = true;
        }

        for (q = p; q < (uint8_t*) p + n; q ++)
                *q = rand();
}