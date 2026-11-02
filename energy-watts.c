#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_SYSFS_ROOT "/sys/class/powercap"
#define PACKAGE_ZONE "intel-rapl:0"
#define SAMPLE_SECONDS 2

static void make_path(char *dst, size_t size, const char *root,
                      const char *file)
{
    int n = snprintf(dst, size, "%s/%s/%s", root, PACKAGE_ZONE, file);
    if (n < 0 || (size_t)n >= size) {
        fprintf(stderr, "Cesta k čítači energie je příliš dlouhá.\n");
        exit(EXIT_FAILURE);
    }
}

static uint64_t read_u64(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Nelze otevřít %s: %s\n", path, strerror(errno));
        exit(EXIT_FAILURE);
    }

    char buf[64];
    if (!fgets(buf, sizeof(buf), f)) {
        fprintf(stderr, "Nelze přečíst %s.\n", path);
        fclose(f);
        exit(EXIT_FAILURE);
    }
    fclose(f);

    errno = 0;
    char *end;
    unsigned long long value = strtoull(buf, &end, 10);
    if (errno || end == buf || (*end != '\n' && *end != '\0')) {
        fprintf(stderr, "Neplatná hodnota v %s.\n", path);
        exit(EXIT_FAILURE);
    }
    return (uint64_t)value;
}

static struct timespec monotonic_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("Nelze zjistit monotónní čas");
        exit(EXIT_FAILURE);
    }
    return ts;
}

static double elapsed_seconds(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

int main(void)
{
    const char *root = getenv("ENERGY_SYSFS_ROOT");
    if (!root || !*root)
        root = DEFAULT_SYSFS_ROOT;

    char energy_path[512];
    char range_path[512];
    make_path(energy_path, sizeof(energy_path), root, "energy_uj");
    make_path(range_path, sizeof(range_path), root, "max_energy_range_uj");

    uint64_t range = read_u64(range_path);
    uint64_t first = read_u64(energy_path);
    struct timespec start = monotonic_now();

    struct timespec remaining = { .tv_sec = SAMPLE_SECONDS, .tv_nsec = 0 };
    while (nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) {
            perror("Čekání na druhé měření selhalo");
            return EXIT_FAILURE;
        }
    }

    uint64_t second = read_u64(energy_path);
    struct timespec end = monotonic_now();
    if (range == 0 || first >= range || second >= range) {
        fprintf(stderr, "Čítač energie vrátil hodnotu mimo rozsah.\n");
        return EXIT_FAILURE;
    }

    uint64_t delta = second >= first ? second - first : range - first + second;
    double watts = (double)delta / 1000000.0 / elapsed_seconds(start, end);
    printf("%.2f W\n", watts);
    return EXIT_SUCCESS;
}
