// SPDX-FileCopyrightText: 2026 Scitrera LLC
// SPDX-FileCopyrightText: 2026 Fox Engine Ltd
// SPDX-License-Identifier: GPL-2.0-only

//
// Restore the bounded NVIDIA control-plane residue left after ColdSnap has
// cooperatively destroyed every CUDA context and externalized device memory.
// Device pages and CUDA state are never checkpointed by this plugin.

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unistd.h>

#include "criu-plugin.h"

#define EXTERNAL_MAP_ENVIRONMENT "COLDSNAP_CRIU_NVIDIA_EXTERNAL_FILE_MAP"
#define EXTERNAL_MAP_HEADER "coldsnap-nvidia-external-files-v1\n"
#define PLACEHOLDER_FDS_ENVIRONMENT "COLDSNAP_CRIU_NVIDIA_PLACEHOLDER_FDS"
#define FD_BROKER_ENVIRONMENT "COLDSNAP_CRIU_NVIDIA_FD_BROKER_SOCKET"
#define FD_BROKER_PROTOCOL "COLDSNAP_FD_BROKER_V1"
#define ZERO_VMA_ENVIRONMENT "COLDSNAP_CRIU_NVIDIA_ZERO_VMAS"

static const char* const nvidia_devices[] = {
    "/dev/nvidia0",
    "/dev/nvidiactl",
    "/dev/nvidia-uvm",
    "/dev/nvidia-uvm-tools",
    "/dev/nvidia-modeset",
};

static int plugin_init(int stage) {
    (void)stage;
    return 0;
}

static void plugin_exit(int stage, int result) {
    (void)stage;
    (void)result;
}

CR_PLUGIN_REGISTER("coldsnap-nvidia-reset-epoch-v1", plugin_init, plugin_exit)

static int is_nvidia_device_stat(const struct stat* target) {
    if (target == NULL || !S_ISCHR(target->st_mode)) {
        return 0;
    }
    for (size_t index = 0;
         index < sizeof(nvidia_devices) / sizeof(nvidia_devices[0]);
         ++index) {
        struct stat candidate;
        if (stat(nvidia_devices[index], &candidate) == 0 &&
            S_ISCHR(candidate.st_mode) &&
            candidate.st_rdev == target->st_rdev) {
            return 1;
        }
    }
    return 0;
}

static int is_numbered_nvidia_device(const char* path) {
    const char prefix[] = "/dev/nvidia";
    if (strncmp(path, prefix, sizeof(prefix) - 1) != 0) {
        return 0;
    }
    path += sizeof(prefix) - 1;
    if (!isdigit((unsigned char)*path)) {
        return 0;
    }
    while (isdigit((unsigned char)*path)) {
        ++path;
    }
    return *path == '\0';
}

static int is_nvidia_device_path(const char* path) {
    if (path == NULL) {
        return 0;
    }
    while (path[0] == '/' && path[1] == '/') {
        ++path;
    }
    for (size_t index = 0;
         index < sizeof(nvidia_devices) / sizeof(nvidia_devices[0]);
         ++index) {
        if (strcmp(path, nvidia_devices[index]) == 0) {
            return 1;
        }
    }
    return is_numbered_nvidia_device(path);
}

static int is_nvidia_external_path(const char* path) {
    return is_nvidia_device_path(path) ||
           strcmp(path, "/dev/nvidia-caps/nvidia-cap1") == 0 ||
           strcmp(path, "/dev/nvidia-caps/nvidia-cap2") == 0;
}

static int parse_external_record(
    const char* line,
    unsigned int* id,
    int* flags,
    unsigned int* device_major,
    unsigned int* device_minor,
    char* path,
    size_t path_bytes
) {
    int consumed = 0;
    if (path_bytes < 256 ||
        sscanf(
            line,
            "%u\t%d\t%u\t%u\t%255s%n",
            id,
            flags,
            device_major,
            device_minor,
            path,
            &consumed
        ) != 5) {
        return -EINVAL;
    }
    for (const char* suffix = line + consumed; *suffix != '\0'; ++suffix) {
        if (!isspace((unsigned char)*suffix)) {
            return -EINVAL;
        }
    }
    return 0;
}

static int connect_fd_broker(void) {
    const char* path = getenv(FD_BROKER_ENVIRONMENT);
    if (path == NULL || path[0] != '/' || strlen(path) >= sizeof(((struct sockaddr_un*)0)->sun_path)) {
        return -EINVAL;
    }
    int descriptor = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
        return -errno;
    }
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (connect(descriptor, (const struct sockaddr*)&address, sizeof(address)) != 0) {
        int result = -errno;
        close(descriptor);
        return result;
    }
    return descriptor;
}

static int broker_get_fd(char kind, uint64_t id) {
    int socket_fd = connect_fd_broker();
    if (socket_fd < 0) {
        return socket_fd;
    }
    char request[128];
    int request_bytes = snprintf(
        request,
        sizeof(request),
        FD_BROKER_PROTOCOL " GET %c %llu\n",
        kind,
        (unsigned long long)id
    );
    if (request_bytes <= 0 || (size_t)request_bytes >= sizeof(request)) {
        close(socket_fd);
        return -EINVAL;
    }
    if (send(socket_fd, request, (size_t)request_bytes, MSG_NOSIGNAL) != request_bytes) {
        int result = errno == 0 ? -EIO : -errno;
        close(socket_fd);
        return result;
    }
    char response[64];
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec vector = {
        .iov_base = response,
        .iov_len = sizeof(response) - 1,
    };
    struct msghdr message = {
        .msg_iov = &vector,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    ssize_t response_bytes = recvmsg(socket_fd, &message, MSG_CMSG_CLOEXEC);
    int saved_errno = errno;
    close(socket_fd);
    if (response_bytes < 0) {
        return -saved_errno;
    }
    response[response_bytes] = '\0';
    if (strncmp(response, "OK", 2) != 0 || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
        return -EIO;
    }
    struct cmsghdr* header = CMSG_FIRSTHDR(&message);
    if (header == NULL || header->cmsg_level != SOL_SOCKET ||
        header->cmsg_type != SCM_RIGHTS || header->cmsg_len != CMSG_LEN(sizeof(int))) {
        return -EIO;
    }
    int descriptor = -1;
    memcpy(&descriptor, CMSG_DATA(header), sizeof(descriptor));
    return descriptor;
}

static int open_external_device(unsigned int wanted_id) {
    const char* map_path = getenv(EXTERNAL_MAP_ENVIRONMENT);
    if (map_path == NULL || map_path[0] != '/') {
        return -EINVAL;
    }
    FILE* stream = fopen(map_path, "re");
    if (stream == NULL) {
        return -errno;
    }
    int result = -ENOENT;
    char line[512];
    if (fgets(line, sizeof(line), stream) == NULL ||
        strcmp(line, EXTERNAL_MAP_HEADER) != 0) {
        result = -EINVAL;
        goto out;
    }
    while (fgets(line, sizeof(line), stream) != NULL) {
        unsigned int id = 0;
        int flags = 0;
        unsigned int expected_major = 0;
        unsigned int expected_minor = 0;
        char path[256];
        result = parse_external_record(
            line,
            &id,
            &flags,
            &expected_major,
            &expected_minor,
            path,
            sizeof(path)
        );
        if (result != 0) {
            goto out;
        }
        if (id != wanted_id) {
            result = -ENOENT;
            continue;
        }
        if (!is_nvidia_external_path(path) ||
            (flags & ~(O_ACCMODE | O_NONBLOCK)) != 0) {
            result = -EPERM;
            goto out;
        }
        int descriptor;
        if (getenv(PLACEHOLDER_FDS_ENVIRONMENT) != NULL) {
            char placeholder[256];
            int bytes = snprintf(
                placeholder,
                sizeof(placeholder),
                "/tmp/coldsnap-nvidia-placeholder-%u-XXXXXX",
                wanted_id
            );
            if (bytes <= 0 || (size_t)bytes >= sizeof(placeholder)) {
                result = -ENAMETOOLONG;
                goto out;
            }
            int seed = mkstemp(placeholder);
            if (seed < 0) {
                result = -errno;
                goto out;
            }
            close(seed);
            descriptor = open(
                placeholder,
                flags | O_CLOEXEC | O_NOFOLLOW
            );
            int open_errno = errno;
            if (unlink(placeholder) != 0 && descriptor >= 0) {
                open_errno = errno;
                close(descriptor);
                descriptor = -1;
            }
            if (descriptor < 0) {
                result = -open_errno;
                goto out;
            }
            result = descriptor;
            goto out;
        } else if (getenv(FD_BROKER_ENVIRONMENT) != NULL) {
            descriptor = broker_get_fd('E', wanted_id);
        } else {
            descriptor = open(path, flags | O_CLOEXEC | O_NOFOLLOW);
        }
        if (descriptor < 0) {
            result = getenv(FD_BROKER_ENVIRONMENT) != NULL ? descriptor : -errno;
            goto out;
        }
        struct stat metadata;
        if (fstat(descriptor, &metadata) != 0) {
            result = -errno;
            close(descriptor);
            goto out;
        }
        if (!S_ISCHR(metadata.st_mode) ||
            major(metadata.st_rdev) != expected_major ||
            minor(metadata.st_rdev) != expected_minor) {
            close(descriptor);
            result = -ENODEV;
            goto out;
        }
        result = descriptor;
        goto out;
    }
    if (ferror(stream)) {
        result = -EIO;
    }
out:
    fclose(stream);
    return result;
}

// Restore NVIDIA descriptors inside CRIU instead of sending SCM_RIGHTS replies
// over the shared RPC service socket. Restore children can request descriptors
// concurrently, while each plugin call independently validates and opens one.
static int restore_external_file(int id, bool* retry_needed) {
    if (id < 0 || retry_needed == NULL) {
        return -EINVAL;
    }
    *retry_needed = false;
    return open_external_device((unsigned int)id);
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__RESTORE_EXT_FILE, restore_external_file)

// Return zero only for NVIDIA character devices. CRIU marks those mappings as
// external-plugin VMAs and omits their device-owned pages from the image.
static int handle_device_vma(int fd, const struct stat* device_stat) {
    if (!is_nvidia_device_stat(device_stat)) {
        return -ENOTSUP;
    }
    (void)fd;
    return 0;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__HANDLE_DEVICE_VMA, handle_device_vma)

// A broker-backed restore reuses the exact struct file that created each VMA;
// NVIDIA rejects mmap cookie replay on a newly opened device file. Without a
// broker, preserve only the CPU address-space shape with a /dev/zero fallback.
static int update_vma_map(
    const char* path,
    uint64_t address,
    uint64_t old_offset,
    uint64_t* new_offset,
    int* plugin_fd
) {
    (void)address;
    if (!is_nvidia_device_path(path)) {
        return -ENOTSUP;
    }
    if (new_offset == NULL || plugin_fd == NULL) {
        return -EINVAL;
    }
    const char* zero_vmas = getenv(ZERO_VMA_ENVIRONMENT);
    if (zero_vmas != NULL && strcmp(zero_vmas, "1") == 0) {
        int descriptor = open("/dev/zero", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor < 0) {
            return -errno;
        }
        *new_offset = 0;
        *plugin_fd = descriptor;
        return 1;
    }
    if (getenv(FD_BROKER_ENVIRONMENT) != NULL) {
        int descriptor = broker_get_fd('M', address);
        if (descriptor < 0) {
            return descriptor;
        }
        struct stat metadata;
        if (fstat(descriptor, &metadata) != 0 || !is_nvidia_device_stat(&metadata)) {
            int result = errno == 0 ? -ENODEV : -errno;
            close(descriptor);
            return result;
        }
        *new_offset = old_offset;
        *plugin_fd = descriptor;
        return 1;
    }
    (void)old_offset;
    int descriptor = open("/dev/zero", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return -errno;
    }
    *new_offset = 0;
    *plugin_fd = descriptor;
    return 1;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__UPDATE_VMA_MAP, update_vma_map)
