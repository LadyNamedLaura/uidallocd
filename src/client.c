
#include <systemd/sd-bus.h>
#include <errno.h>
#include <alloca.h>
#include "util.h"


int safe_atollu(const char *s, uint64_t *ret_llu) {
        char *x = NULL;
        unsigned long long l;

        assert(s);
        assert(ret_llu);

        errno = 0;
        l = strtoull(s, &x, 0);

        if (!x || x == s || *x || errno)
                return errno ? -errno : -EINVAL;

        *ret_llu = l;
        return 0;
}
void help() {
        printf("uidalloc alloc COUNT [ALIAS]\n"
               "uidalloc release {ID|alias=ALIAS}\n");
}

int main(int argc, char *argv[]) {
        int r;
        sd_bus *bus;
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message *reply = NULL;
        char *path;

        r = sd_bus_default_user(&bus);
        if (r < 0) {
                log_error("Failed to connect to the bus: %s", strerror(-r));
                goto end;
        }

        if (argc < 3) {
                help();
                return EXIT_FAILURE;
        }

        if (streq("alloc",argv[1])) {
                uint64_t size;
                uint64_t reply_size;
                uint64_t reply_start;
                const char *alias = "";

                r = safe_atollu(argv[2], &size);

                if (argv[3])
                        alias = argv[3];

                r = sd_bus_call_method(
                        bus, "be.enospc.uidallocd", "/be/enospc/uidallocd", "be.enospc.uidallocd.Manager",
                        "AllocUids", &err, &reply, "stb", alias, size, false);
                if (r < 0) {
                        log_error("Failed to alloc uids: %s", strerror(-r));
                        goto end;
                }
                r = sd_bus_message_read(reply, "ott", &path, &reply_start, &reply_size);
                if (r < 0) {
                        log_error("Failed to read reply: %s", strerror(-r));
                        return r;
                }
                printf("got reply, start: %lu, size: %lu (%s)\n", reply_start, reply_size,path);                
        } else if (streq("release",argv[1])) {
                const char *id = argv[2];
                const char *alias;

                path = newa0(char, strlen(id)+ strlen("/be/enospc/uidallocd/leases/")+1);
                alias = startswith(id, "alias=");
                if (alias) {
                        strcat(path, "/be/enospc/uidallocd/aliases/");
                        strcat(path, alias);
                } else {
                        strcat(path, "/be/enospc/uidallocd/leases/");
                        strcat(path, id);
                }

                r = sd_bus_call_method(
                        bus, "be.enospc.uidallocd", path, "be.enospc.uidallocd.Lease",
                        "Release", &err, &reply, "");
                if (r < 0) {
                        log_error("Failed to release uids: %s", strerror(-r));
                        goto end;
                }

        }

end:
        if (r < 0)
                return EXIT_FAILURE;
        return EXIT_SUCCESS;
}