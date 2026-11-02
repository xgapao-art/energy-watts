#define _POSIX_C_SOURCE 200809L

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SYSFS_ROOT "/sys/class/powercap"
#define PACKAGE_ZONE "intel-rapl:0"
#define SAMPLE_SECONDS 2
#define DGPU_SAMPLE_SECONDS 10

typedef struct nvmlDevice_st *nvml_device_t;
typedef int (*nvml_init_fn)(void);
typedef int (*nvml_shutdown_fn)(void);
typedef int (*nvml_device_count_fn)(unsigned int *);
typedef int (*nvml_device_by_index_fn)(unsigned int, nvml_device_t *);
typedef int (*nvml_power_usage_fn)(nvml_device_t, unsigned int *);

typedef struct {
    void *library;
    nvml_shutdown_fn shutdown;
    nvml_device_count_fn device_count;
    nvml_device_by_index_fn device_by_index;
    nvml_power_usage_fn power_usage;
} nvml_api_t;

static void load_nvml_symbol(void *library, const char *name,
                             void *function, size_t function_size)
{
    void *symbol = dlsym(library, name);
    if (!symbol || function_size != sizeof(symbol)) {
        fprintf(stderr, "Nelze načíst funkci NVML %s.\n", name);
        exit(EXIT_FAILURE);
    }
    memcpy(function, &symbol, function_size);
}

static nvml_api_t open_nvml(void)
{
    nvml_api_t api = { 0 };
    api.library = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!api.library) {
        fprintf(stderr, "Nelze načíst knihovnu NVML: %s\n", dlerror());
        exit(EXIT_FAILURE);
    }

    nvml_init_fn init;
    load_nvml_symbol(api.library, "nvmlInit_v2", &init, sizeof(init));
    load_nvml_symbol(api.library, "nvmlShutdown", &api.shutdown,
                     sizeof(api.shutdown));
    load_nvml_symbol(api.library, "nvmlDeviceGetCount_v2", &api.device_count,
                     sizeof(api.device_count));
    load_nvml_symbol(api.library, "nvmlDeviceGetHandleByIndex_v2",
                     &api.device_by_index, sizeof(api.device_by_index));
    load_nvml_symbol(api.library, "nvmlDeviceGetPowerUsage", &api.power_usage,
                     sizeof(api.power_usage));

    int result = init();
    if (result != 0) {
        fprintf(stderr, "Inicializace NVML selhala (%d).\n", result);
        dlclose(api.library);
        exit(EXIT_FAILURE);
    }
    return api;
}

static double read_dgpu_watts(const nvml_api_t *api)
{
    unsigned int count;
    int result = api->device_count(&count);
    if (result != 0 || count == 0) {
        fprintf(stderr, "Nelze zjistit NVIDIA GPU přes NVML (%d).\n", result);
        exit(EXIT_FAILURE);
    }

    unsigned long long total_milliwatts = 0;
    for (unsigned int i = 0; i < count; i++) {
        nvml_device_t device;
        unsigned int milliwatts;
        result = api->device_by_index(i, &device);
        if (result == 0)
            result = api->power_usage(device, &milliwatts);
        if (result != 0) {
            fprintf(stderr, "Nelze přečíst příkon NVIDIA GPU (%d).\n", result);
            exit(EXIT_FAILURE);
        }
        total_milliwatts += milliwatts;
    }
    return (double)total_milliwatts / 1000.0;
}

static int read_sysfs_text(const char *path, char *value, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    int found = fgets(value, size, f) != NULL;
    fclose(f);
    if (!found)
        return 0;

    value[strcspn(value, "\r\n")] = '\0';
    return 1;
}

static int nvidia_gpus_are_active(void)
{
    glob_t devices;
    int result = glob("/sys/class/drm/card[0-9]*/device/vendor", 0, NULL,
                      &devices);
    if (result != 0)
        return 0;

    int found = 0;
    int all_active = 1;
    for (size_t i = 0; i < devices.gl_pathc; i++) {
        char vendor[64];
        if (!read_sysfs_text(devices.gl_pathv[i], vendor, sizeof(vendor))) {
            all_active = 0;
            break;
        }
        if (strcmp(vendor, "0x10de") != 0)
            continue;

        found = 1;
        const char *slash = strrchr(devices.gl_pathv[i], '/');
        char status_path[512];
        int n = snprintf(status_path, sizeof(status_path), "%.*s/power/runtime_status",
                         (int)(slash - devices.gl_pathv[i]), devices.gl_pathv[i]);
        char status[64];
        if (n < 0 || (size_t)n >= sizeof(status_path) ||
            !read_sysfs_text(status_path, status, sizeof(status)) ||
            strcmp(status, "active") != 0) {
            all_active = 0;
            break;
        }
    }
    globfree(&devices);
    return found && all_active;
}

static double sample_dgpu_watts(void)
{
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir || !*runtime_dir)
        return 0.0;

    char path[512];
    int n = snprintf(path, sizeof(path), "%s/energy-watts-dgpu.cache", runtime_dir);
    if (n < 0 || (size_t)n >= sizeof(path))
        return 0.0;

    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (fd < 0)
        return 0.0;
    if (flock(fd, LOCK_EX) != 0) {
        close(fd);
        return 0.0;
    }
    FILE *cache = fdopen(fd, "r+");
    if (!cache) {
        close(fd);
        return 0.0;
    }

    long long last_sample = -1;
    long last_nanoseconds = 0;
    int valid = 0;
    double watts = 0.0;
    if (fscanf(cache, "%lld %ld %d %lf", &last_sample, &last_nanoseconds,
               &valid, &watts) != 4 || last_nanoseconds < 0 ||
        last_nanoseconds >= 1000000000L ||
        (valid != 0 && valid != 1) || !(watts >= 0.0)) {
        last_sample = -1;
        last_nanoseconds = 0;
        valid = 0;
        watts = 0.0;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        fclose(cache);
        return 0.0;
    }
    if (last_sample > (long long)now.tv_sec ||
        (last_sample == (long long)now.tv_sec && last_nanoseconds > now.tv_nsec))
        last_sample = -1;

    if (!nvidia_gpus_are_active()) {
        valid = 0;
        watts = 0.0;
    } else if (last_sample < 0 ||
               (long long)now.tv_sec - last_sample > DGPU_SAMPLE_SECONDS ||
               ((long long)now.tv_sec - last_sample == DGPU_SAMPLE_SECONDS &&
                now.tv_nsec >= last_nanoseconds)) {
        nvml_api_t api = open_nvml();
        watts = read_dgpu_watts(&api);
        api.shutdown();
        dlclose(api.library);
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            fclose(cache);
            return 0.0;
        }
        last_sample = (long long)now.tv_sec;
        last_nanoseconds = now.tv_nsec;
        valid = 1;
    }

    rewind(cache);
    if (ftruncate(fd, 0) != 0 ||
        fprintf(cache, "%lld %ld %d %.17g\n", last_sample, last_nanoseconds,
            valid, watts) < 0 ||
        fflush(cache) != 0) {
        fclose(cache);
        return 0.0;
    }
    fclose(cache);
    if (!valid)
        return 0.0;
    return watts;
}

static void make_path(char *dst, size_t size, const char *root,
                      const char *file)
{
    int n = snprintf(dst, size, "%s/%s/%s", root, PACKAGE_ZONE, file);
    if (n < 0 || (size_t)n >= size) {
        fprintf(stderr, "Cesta k čítači energie je příliš dlouhá.\n");
        exit(EXIT_FAILURE);
    }
}

static int is_amdgpu_power_path(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
        return 0;

    char name_path[512];
    int n = snprintf(name_path, sizeof(name_path), "%.*s/name",
                     (int)(slash - path), path);
    if (n < 0 || (size_t)n >= sizeof(name_path))
        return 0;

    FILE *f = fopen(name_path, "r");
    if (!f)
        return 0;

    char name[64];
    int found = fgets(name, sizeof(name), f) != NULL;
    fclose(f);
    if (!found)
        return 0;

    name[strcspn(name, "\r\n")] = '\0';
    return strcmp(name, "amdgpu") == 0;
}

static void find_igpu_power_path(char *dst, size_t size)
{
    const char *override = getenv("ENERGY_IGPU_POWER_PATH");
    if (override && *override) {
        int n = snprintf(dst, size, "%s", override);
        if (n < 0 || (size_t)n >= size) {
            fprintf(stderr, "Cesta k čítači iGPU je příliš dlouhá.\n");
            exit(EXIT_FAILURE);
        }
        return;
    }

    glob_t matches;
    int result = glob("/sys/class/drm/card[0-9]*/device/hwmon/hwmon*/power1_average",
                      0, NULL, &matches);
    if (result == 0) {
        for (size_t i = 0; i < matches.gl_pathc; i++) {
            if (!is_amdgpu_power_path(matches.gl_pathv[i]))
                continue;

            int n = snprintf(dst, size, "%s", matches.gl_pathv[i]);
            globfree(&matches);
            if (n < 0 || (size_t)n >= size) {
                fprintf(stderr, "Cesta k čítači iGPU je příliš dlouhá.\n");
                exit(EXIT_FAILURE);
            }
            return;
        }
        globfree(&matches);
    }

    fprintf(stderr,
            "Nelze najít příkon iGPU (amdgpu power1_average). "
            "Cestu lze nastavit proměnnou ENERGY_IGPU_POWER_PATH.\n");
    exit(EXIT_FAILURE);
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
    char igpu_power_path[512];
    make_path(energy_path, sizeof(energy_path), root, "energy_uj");
    make_path(range_path, sizeof(range_path), root, "max_energy_range_uj");
    find_igpu_power_path(igpu_power_path, sizeof(igpu_power_path));
    uint64_t range = read_u64(range_path);
    uint64_t first = read_u64(energy_path);
    uint64_t igpu_first = read_u64(igpu_power_path);
    double dgpu_first = sample_dgpu_watts();
    struct timespec start = monotonic_now();

    struct timespec remaining = { .tv_sec = SAMPLE_SECONDS, .tv_nsec = 0 };
    while (nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) {
            perror("Čekání na druhé měření selhalo");
            return EXIT_FAILURE;
        }
    }

    uint64_t second = read_u64(energy_path);
    uint64_t igpu_second = read_u64(igpu_power_path);
    double dgpu_second = sample_dgpu_watts();
    struct timespec end = monotonic_now();
    if (range == 0 || first >= range || second >= range) {
        fprintf(stderr, "Čítač energie vrátil hodnotu mimo rozsah.\n");
        return EXIT_FAILURE;
    }

    uint64_t delta = second >= first ? second - first : range - first + second;
    double cpu_watts = (double)delta / 1000000.0 /
                       elapsed_seconds(start, end);
    double igpu_watts = ((double)igpu_first + (double)igpu_second) /
                        2000000.0;
        printf("%.1f W\n", cpu_watts + igpu_watts +
            (dgpu_first + dgpu_second) / 2.0);
    return EXIT_SUCCESS;
}
