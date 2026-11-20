#include "transport.h"
#include "wire.h"
#include "pixel_input.h"

#include <CoreMedia/CMBlockBuffer.h>
#include <VideoToolbox/VTErrors.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    kTransportTimeoutSeconds = 30,
    kProbeTimeoutSeconds = 8,
    kWorkerCloseWaitMilliseconds = 100,
    kMaximumWorkers = 4
};

typedef struct {
    pid_t child;
} WEXReaperArgument;

static pthread_mutex_t gWorkerLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned gWorkerCount;
static pid_t gUnreapedChildren[kMaximumWorkers];
static pthread_once_t gAvailabilityOnce = PTHREAD_ONCE_INIT;
static bool gBackendAvailable;
static WEXProbeInfo gBackendProbeInfo;

struct WEXEncoderBackend {
    pthread_mutex_t lock;
    pid_t child;
    int socket;
    int32_t width;
    int32_t height;
    bool unusable;
};

static int set_close_on_exec(int descriptor) {
    int flags = fcntl(descriptor, F_GETFD);
    return flags < 0 || fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0
        ? -1 : 0;
}

static bool trusted_owner(uid_t owner) {
    return owner == 0 || owner == geteuid();
}

static bool trusted_directory(const char *path) {
    struct stat info;
    if (lstat(path, &info) < 0 || !S_ISDIR(info.st_mode) ||
        !trusted_owner(info.st_uid) || (info.st_mode & 022) != 0) {
        return false;
    }
    return true;
}

static bool trusted_directory_chain(const char *path) {
    if (!path || path[0] != '/') {
        return false;
    }

    size_t length = strnlen(path, PATH_MAX);
    if (length == 0 || length >= PATH_MAX) {
        return false;
    }

    char candidate[PATH_MAX];
    memcpy(candidate, path, length + 1);
    for (size_t index = 1; index <= length; ++index) {
        if (candidate[index] != '/' && candidate[index] != '\0') {
            continue;
        }
        char delimiter = candidate[index];
        candidate[index] = '\0';
        bool trusted = trusted_directory(candidate);
        candidate[index] = delimiter;
        if (!trusted) {
            return false;
        }
    }
    return true;
}

static bool worker_path(char output[PATH_MAX]) {
    Dl_info image = {0};
    if (!dladdr((const void *)&worker_path, &image) ||
        !image.dli_fname || !image.dli_fname[0]) {
        return false;
    }

    char resolved_image[PATH_MAX];
    if (!realpath(image.dli_fname, resolved_image)) {
        return false;
    }

    char *last_slash = strrchr(resolved_image, '/');
    if (!last_slash || last_slash == resolved_image) {
        return false;
    }
    *last_slash = '\0';

    char backend_bundle[PATH_MAX];
    char contents[PATH_MAX];
    char macos[PATH_MAX];
    int first = snprintf(backend_bundle, sizeof(backend_bundle),
                         "%s/WEXHEVCBackend", resolved_image);
    int second = snprintf(contents, sizeof(contents),
                          "%s/Contents", backend_bundle);
    int third = snprintf(macos, sizeof(macos), "%s/MacOS", contents);
    int final = snprintf(output, PATH_MAX, "%s/vt_client", macos);
    if (first < 0 || (size_t)first >= sizeof(backend_bundle) ||
        second < 0 || (size_t)second >= sizeof(contents) ||
        third < 0 || (size_t)third >= sizeof(macos) ||
        final < 0 || final >= PATH_MAX) {
        return false;
    }

    if (!trusted_directory_chain(macos)) {
        return false;
    }

    struct stat info;
    if (lstat(output, &info) < 0 || !S_ISREG(info.st_mode) ||
        !trusted_owner(info.st_uid) || (info.st_mode & 022) != 0 ||
        (info.st_mode & 0111) == 0) {
        return false;
    }

    char resolved_worker[PATH_MAX];
    return realpath(output, resolved_worker) &&
        strcmp(output, resolved_worker) == 0;
}

static bool set_nonblocking(int descriptor) {
    int flags = fcntl(descriptor, F_GETFL);
    return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void reap_children_without_blocking(void) {
    pthread_mutex_lock(&gWorkerLock);
    for (size_t index = 0; index < kMaximumWorkers; ++index) {
        pid_t child = gUnreapedChildren[index];
        if (child <= 0) {
            continue;
        }
        pid_t result = waitpid(child, NULL, WNOHANG);
        if (result == child || (result < 0 && errno == ECHILD)) {
            gUnreapedChildren[index] = -1;
            if (gWorkerCount > 0) {
                --gWorkerCount;
            }
        }
    }
    pthread_mutex_unlock(&gWorkerLock);
}

static bool reserve_worker_slot(void) {
    reap_children_without_blocking();
    pthread_mutex_lock(&gWorkerLock);
    bool available = gWorkerCount < kMaximumWorkers;
    if (available) {
        ++gWorkerCount;
    }
    pthread_mutex_unlock(&gWorkerLock);
    return available;
}

static void release_worker_slot(void) {
    pthread_mutex_lock(&gWorkerLock);
    if (gWorkerCount > 0) {
        --gWorkerCount;
    }
    pthread_mutex_unlock(&gWorkerLock);
}

static void *reap_child_thread(void *argument) {
    WEXReaperArgument *reaper = argument;
    pid_t child = reaper->child;
    free(reaper);
    for (;;) {
        pid_t result = waitpid(child, NULL, 0);
        if (result == child || (result < 0 && errno == ECHILD)) {
            break;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000L};
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
        }
    }
    release_worker_slot();
    return NULL;
}

static void transfer_child_to_reaper(pid_t child) {
    WEXReaperArgument *argument = malloc(sizeof(*argument));
    if (argument) {
        argument->child = child;
        pthread_t thread;
        pthread_attr_t attributes;
        int status = pthread_attr_init(&attributes);
        bool attributes_initialized = status == 0;
        if (status == 0) {
            status = pthread_attr_setdetachstate(&attributes,
                                                 PTHREAD_CREATE_DETACHED);
        }
        if (status == 0) {
            status = pthread_create(&thread, &attributes, reap_child_thread,
                                    argument);
        }
        if (attributes_initialized) {
            pthread_attr_destroy(&attributes);
        }
        if (status == 0) {
            return;
        }
        free(argument);
    }

    pid_t result = waitpid(child, NULL, WNOHANG);
    if (result == child || (result < 0 && errno == ECHILD)) {
        release_worker_slot();
        return;
    }

    pthread_mutex_lock(&gWorkerLock);
    for (size_t index = 0; index < kMaximumWorkers; ++index) {
        if (gUnreapedChildren[index] <= 0) {
            gUnreapedChildren[index] = child;
            break;
        }
    }
    pthread_mutex_unlock(&gWorkerLock);
}

static bool wait_for_child_briefly(pid_t child) {
    int remaining = kWorkerCloseWaitMilliseconds;
    while (remaining > 0) {
        pid_t result = waitpid(child, NULL, WNOHANG);
        if (result == child || (result < 0 && errno == ECHILD)) {
            return true;
        }
        const int pause = remaining < 10 ? remaining : 10;
        struct timespec delay = {.tv_sec = 0, .tv_nsec = pause * 1000000L};
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
        }
        remaining -= pause;
    }
    return false;
}

static bool move_descriptor_above_stdio(int *descriptor) {
    if (*descriptor >= STDERR_FILENO + 1) {
        return true;
    }
    int moved = fcntl(*descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (moved < 0) {
        return false;
    }
    close(*descriptor);
    *descriptor = moved;
    return true;
}

static struct timespec deadline_after_seconds(int seconds) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += seconds;
    return deadline;
}

static int remaining_milliseconds(const struct timespec *deadline) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    int64_t nanoseconds =
        (int64_t)(deadline->tv_sec - now.tv_sec) * 1000000000LL +
        (int64_t)deadline->tv_nsec - now.tv_nsec;
    if (nanoseconds <= 0) {
        return 0;
    }
    int64_t milliseconds = (nanoseconds + 999999LL) / 1000000LL;
    return milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
}

static bool wait_for_socket(int descriptor, short events,
                            const struct timespec *deadline) {
    for (;;) {
        int timeout = remaining_milliseconds(deadline);
        if (timeout == 0) {
            errno = ETIMEDOUT;
            return false;
        }

        struct pollfd item = {.fd = descriptor, .events = events, .revents = 0};
        int result = poll(&item, 1, timeout);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            if (result == 0) {
                errno = ETIMEDOUT;
            }
            return false;
        }
        if (item.revents & events) {
            return true;
        }
        errno = (item.revents & POLLNVAL) ? EBADF : ECONNRESET;
        return false;
    }
}

static bool write_exact(int descriptor, const void *buffer, size_t bytes,
                        const struct timespec *deadline) {
    const uint8_t *position = buffer;
    size_t remaining = bytes;
    while (remaining) {
        if (!wait_for_socket(descriptor, POLLOUT, deadline)) {
            return false;
        }
        ssize_t count = send(descriptor, position, remaining, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN ||
                          errno == EWOULDBLOCK)) {
            continue;
        }
        if (count <= 0) {
            if (count == 0) {
                errno = EPIPE;
            }
            return false;
        }
        position += count;
        remaining -= (size_t)count;
    }
    return true;
}

static bool read_exact(int descriptor, void *buffer, size_t bytes,
                       const struct timespec *deadline) {
    uint8_t *position = buffer;
    size_t remaining = bytes;
    while (remaining) {
        if (!wait_for_socket(descriptor, POLLIN, deadline)) {
            return false;
        }
        ssize_t count = recv(descriptor, position, remaining, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN ||
                          errno == EWOULDBLOCK)) {
            continue;
        }
        if (count <= 0) {
            if (count == 0) {
                errno = ECONNRESET;
            }
            return false;
        }
        position += count;
        remaining -= (size_t)count;
    }
    return true;
}

static WEXMessage message(uint16_t kind, uint32_t payload_bytes) {
    WEXMessage result = {
        .magic = WEXWireMagic,
        .version = WEXWireVersion,
        .kind = kind,
        .payloadBytes = payload_bytes,
        .status = 0
    };
    return result;
}

static bool send_message(int descriptor, uint16_t kind,
                         const void *payload, uint32_t payload_bytes,
                         const struct timespec *deadline) {
    WEXMessage header = message(kind, payload_bytes);
    return write_exact(descriptor, &header, sizeof(header), deadline) &&
        (!payload_bytes || write_exact(descriptor, payload, payload_bytes,
                                       deadline));
}

static OSStatus receive_reply(int descriptor, uint16_t expected_kind,
                              WEXMessage *reply,
                              const struct timespec *deadline) {
    if (!read_exact(descriptor, reply, sizeof(*reply), deadline)) {
        return kVTVideoEncoderMalfunctionErr;
    }
    if (reply->magic != WEXWireMagic || reply->version != WEXWireVersion ||
        reply->kind != expected_kind || reply->payloadBytes > WEXMaxPacket) {
        return kVTVideoEncoderMalfunctionErr;
    }
    if (reply->status != noErr) {
        return reply->payloadBytes == 0
            ? (OSStatus)reply->status : kVTVideoEncoderMalfunctionErr;
    }
    return noErr;
}

static bool configure_socket(int descriptor) {
    int enabled = 1;
    if (setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE,
                   &enabled, sizeof(enabled)) < 0) {
        return false;
    }
    return true;
}

static void stop_backend(WEXEncoderBackend *backend) {
    if (backend->socket >= 0) {
        shutdown(backend->socket, SHUT_RDWR);
        close(backend->socket);
        backend->socket = -1;
    }
    if (backend->child > 0) {
        pid_t child = backend->child;
        backend->child = -1;
        if (wait_for_child_briefly(child)) {
            release_worker_slot();
        } else {
            transfer_child_to_reaper(child);
        }
    }
    backend->unusable = true;
}

static bool valid_timescale(CMTime time) {
    return CMTIME_IS_NUMERIC(time) && time.timescale > 0;
}

static bool valid_config(int32_t width, int32_t height, int32_t bitrate,
                         int32_t fps_num, int32_t fps_den) {
    if (width <= 0 || height <= 0 || width > WEXMaxWidth ||
        height > WEXMaxHeight || (width & 1) || (height & 1) ||
        bitrate <= 0 || bitrate > 500000000 ||
        fps_num <= 0 || fps_den <= 0 ||
        (int64_t)fps_num > (int64_t)120 * fps_den) {
        return false;
    }
    return true;
}

static bool valid_nal(const uint8_t *data, size_t bytes, uint8_t *type) {
    if (!data || bytes < 2 || (data[0] & 0x80) != 0 ||
        (data[1] & 0x07) == 0) {
        return false;
    }
    *type = (uint8_t)((data[0] >> 1) & 0x3f);
    return true;
}

static bool valid_parameter_set(const uint8_t *data, size_t bytes,
                                uint8_t expected_type) {
    uint8_t type = 0;
    return bytes <= WEXMaxParameterSet &&
        valid_nal(data, bytes, &type) && type == expected_type;
}

static bool valid_coded_access_unit(const uint8_t *data, size_t bytes,
                                    uint32_t length_bytes,
                                    bool is_key_frame) {
    if (!data || !bytes || length_bytes < 1 || length_bytes > 4) {
        return false;
    }

    size_t offset = 0;
    bool has_vcl = false;
    bool has_irap = false;
    while (offset < bytes) {
        if (bytes - offset < length_bytes) {
            return false;
        }
        uint32_t nal_bytes = 0;
        for (uint32_t index = 0; index < length_bytes; ++index) {
            nal_bytes = (nal_bytes << 8) | data[offset + index];
        }
        offset += length_bytes;
        if (nal_bytes == 0 || nal_bytes > bytes - offset) {
            return false;
        }

        uint8_t type = 0;
        if (!valid_nal(data + offset, nal_bytes, &type)) {
            return false;
        }
        has_vcl |= type <= 31;
        has_irap |= type >= 16 && type <= 23;
        offset += nal_bytes;
    }
    return has_vcl && has_irap == is_key_frame;
}

static OSStatus make_sample_buffer(const WEXEncodedInfo *info,
                                   const uint8_t *payload,
                                   CMSampleBufferRef *sample_buffer) {
    if (!info || !payload || !sample_buffer ||
        !valid_timescale((CMTime){.value = info->ptsValue,
                                  .timescale = info->ptsScale,
                                  .flags = kCMTimeFlags_Valid}) ||
        info->durationScale < 0 || info->durationValue < 0 ||
        (info->durationScale == 0 && info->durationValue != 0)) {
        return kVTVideoEncoderMalfunctionErr;
    }

    uint64_t parameter_bytes =
        (uint64_t)info->vpsBytes + info->spsBytes + info->ppsBytes;
    if (!info->vpsBytes || !info->spsBytes || !info->ppsBytes ||
        info->nalLengthBytes < 1 || info->nalLengthBytes > 4 ||
        parameter_bytes > WEXMaxPacket ||
        info->codedBytes == 0 ||
        parameter_bytes + info->codedBytes + sizeof(*info) > WEXMaxPacket) {
        return kVTVideoEncoderMalfunctionErr;
    }

    const uint8_t *vps = payload;
    const uint8_t *sps = vps + info->vpsBytes;
    const uint8_t *pps = sps + info->spsBytes;
    const uint8_t *coded = pps + info->ppsBytes;
    if (!valid_parameter_set(vps, info->vpsBytes, 32) ||
        !valid_parameter_set(sps, info->spsBytes, 33) ||
        !valid_parameter_set(pps, info->ppsBytes, 34) ||
        !valid_coded_access_unit(coded, info->codedBytes,
                                 info->nalLengthBytes, info->keyFrame != 0)) {
        return kVTVideoEncoderMalfunctionErr;
    }

    const uint8_t *parameter_sets[] = {vps, sps, pps};
    const size_t parameter_sizes[] = {
        info->vpsBytes, info->spsBytes, info->ppsBytes
    };
    CMVideoFormatDescriptionRef format = NULL;
    OSStatus status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
        kCFAllocatorDefault, 3, parameter_sets, parameter_sizes,
        info->nalLengthBytes, NULL, &format);
    if (status != noErr) {
        return status;
    }

    CMBlockBufferRef block = NULL;
    status = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault, NULL, info->codedBytes, kCFAllocatorDefault, NULL,
        0, info->codedBytes, kCMBlockBufferAssureMemoryNowFlag, &block);
    if (status == noErr) {
        status = CMBlockBufferReplaceDataBytes(coded, block, 0,
                                               info->codedBytes);
    }

    CMSampleBufferRef output = NULL;
    if (status == noErr) {
        CMTime duration = info->durationScale > 0
            ? CMTimeMake(info->durationValue, info->durationScale)
            : kCMTimeInvalid;
        CMSampleTimingInfo timing = {
            .duration = duration,
            .presentationTimeStamp = CMTimeMake(info->ptsValue, info->ptsScale),
            .decodeTimeStamp = kCMTimeInvalid
        };
        size_t sample_size = info->codedBytes;
        status = CMSampleBufferCreateReady(
            kCFAllocatorDefault, block, format, 1, 1, &timing, 1,
            &sample_size, &output);
    }

    if (block) {
        CFRelease(block);
    }
    CFRelease(format);
    if (status != noErr) {
        if (output) {
            CFRelease(output);
        }
        return status;
    }

    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(output, true);
    if (!attachments || CFArrayGetCount(attachments) != 1) {
        CFRelease(output);
        return kVTVideoEncoderMalfunctionErr;
    }
    CFMutableDictionaryRef attachment =
        (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
    if (info->keyFrame) {
        CFDictionaryRemoveValue(attachment, kCMSampleAttachmentKey_NotSync);
        CFDictionaryRemoveValue(attachment, kCMSampleAttachmentKey_DependsOnOthers);
    } else {
        CFDictionarySetValue(attachment, kCMSampleAttachmentKey_NotSync,
                             kCFBooleanTrue);
        CFDictionarySetValue(attachment,
                             kCMSampleAttachmentKey_DependsOnOthers,
                             kCFBooleanTrue);
    }

    *sample_buffer = output;
    return noErr;
}

static OSStatus spawn_backend_process(WEXEncoderBackend **backend_output) {
    if (!backend_output) {
        return kVTParameterErr;
    }
    *backend_output = NULL;
    char executable[PATH_MAX];
    if (!worker_path(executable)) {
        return kVTVideoEncoderNotAvailableNowErr;
    }

    WEXEncoderBackend *backend = calloc(1, sizeof(*backend));
    if (!backend) {
        return kVTAllocationFailedErr;
    }
    backend->socket = -1;
    backend->child = -1;
    if (pthread_mutex_init(&backend->lock, NULL) != 0) {
        free(backend);
        return kVTAllocationFailedErr;
    }

    if (!reserve_worker_slot()) {
        pthread_mutex_destroy(&backend->lock);
        free(backend);
        return kVTVideoEncoderNotAvailableNowErr;
    }

    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0 ||
        !move_descriptor_above_stdio(&sockets[0]) ||
        !move_descriptor_above_stdio(&sockets[1]) ||
        set_close_on_exec(sockets[0]) < 0 ||
        set_close_on_exec(sockets[1]) < 0 ||
        !configure_socket(sockets[0]) ||
        !configure_socket(sockets[1]) ||
        !set_nonblocking(sockets[0])) {
        if (sockets[0] >= 0) close(sockets[0]);
        if (sockets[1] >= 0) close(sockets[1]);
        release_worker_slot();
        pthread_mutex_destroy(&backend->lock);
        free(backend);
        return kVTVideoEncoderNotAvailableNowErr;
    }

    posix_spawnattr_t attributes;
    int spawn_status = posix_spawnattr_init(&attributes);
    bool attributes_initialized = spawn_status == 0;
    if (spawn_status == 0) {
        spawn_status = posix_spawnattr_setflags(&attributes,
                                                 POSIX_SPAWN_CLOEXEC_DEFAULT);
    }

    posix_spawn_file_actions_t actions;
    if (spawn_status == 0) {
        spawn_status = posix_spawn_file_actions_init(&actions);
    }
    bool actions_initialized = spawn_status == 0;
    if (spawn_status == 0) {
        spawn_status = posix_spawn_file_actions_adddup2(&actions, sockets[1],
                                                        STDIN_FILENO);
    }
    if (spawn_status == 0) {
        spawn_status = posix_spawn_file_actions_addclose(&actions, sockets[0]);
    }
    if (spawn_status == 0) {
        spawn_status = posix_spawn_file_actions_addclose(&actions, sockets[1]);
    }
    if (spawn_status == 0) {
        spawn_status = posix_spawn_file_actions_addinherit_np(&actions,
                                                              STDERR_FILENO);
    }
    if (spawn_status == 0) {
        spawn_status = posix_spawn_file_actions_addopen(
            &actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }

    pid_t child = -1;
    char *const arguments[] = {executable, NULL};
    char *const clean_environment[] = {NULL};
    if (spawn_status == 0) {
        spawn_status = posix_spawn(&child, executable, &actions, &attributes,
                                   arguments, clean_environment);
    }
    if (actions_initialized) {
        posix_spawn_file_actions_destroy(&actions);
    }
    if (attributes_initialized) {
        posix_spawnattr_destroy(&attributes);
    }
    close(sockets[1]);
    sockets[1] = -1;

    if (spawn_status != 0) {
        close(sockets[0]);
        release_worker_slot();
        pthread_mutex_destroy(&backend->lock);
        free(backend);
        return kVTVideoEncoderNotAvailableNowErr;
    }
    backend->child = child;
    backend->socket = sockets[0];

    *backend_output = backend;
    return noErr;
}

static bool valid_probe_info(const WEXProbeInfo *info) {
    return info && info->abiVersion == WEXNativeProbeABIVersion &&
        info->structBytes == sizeof(*info) &&
        info->vendorID == WEXIntelVendorID &&
        info->deviceID == WEXUHD770DeviceID &&
        info->registryID != 0 &&
        (info->capabilities & WEXProbeRequiredCapabilities) ==
            WEXProbeRequiredCapabilities &&
        info->maxWidth >= WEXMaxWidth && info->maxHeight >= WEXMaxHeight &&
        info->reserved == 0;
}

static void probe_backend_once(void) {
    WEXEncoderBackend *backend = NULL;
    OSStatus status = spawn_backend_process(&backend);
    if (status != noErr || !backend) {
        return;
    }

    struct timespec deadline = deadline_after_seconds(kProbeTimeoutSeconds);
    WEXMessage reply;
    status = send_message(backend->socket, WEXProbe, NULL, 0, &deadline)
        ? receive_reply(backend->socket, WEXProbe, &reply, &deadline)
        : kVTVideoEncoderNotAvailableNowErr;

    WEXProbeInfo probe = {0};
    if (status == noErr) {
        if (reply.payloadBytes != sizeof(probe) ||
            !read_exact(backend->socket, &probe, sizeof(probe), &deadline)) {
            status = kVTVideoEncoderMalfunctionErr;
        } else if (!valid_probe_info(&probe)) {
            status = kVTVideoEncoderNotAvailableNowErr;
        }
    }

    gBackendAvailable = status == noErr;
    if (gBackendAvailable) gBackendProbeInfo = probe;
    stop_backend(backend);
    pthread_mutex_destroy(&backend->lock);
    free(backend);
}

bool WEXBackendIsAvailable(uint64_t registryID) {
    if (registryID == 0) return false;
    if (pthread_once(&gAvailabilityOnce, probe_backend_once) != 0) {
        return false;
    }
    return gBackendAvailable && gBackendProbeInfo.registryID == registryID;
}

OSStatus WEXBackendOpen(uint64_t registryID,
                        int32_t width, int32_t height, int32_t bitrate,
                        int32_t fpsNum, int32_t fpsDen,
                        WEXEncoderBackend **backend_output) {
    if (!backend_output) {
        return kVTParameterErr;
    }
    *backend_output = NULL;
    if (registryID == 0 ||
        !valid_config(width, height, bitrate, fpsNum, fpsDen)) {
        return kVTParameterErr;
    }

    WEXEncoderBackend *backend = NULL;
    OSStatus status = spawn_backend_process(&backend);
    if (status != noErr || !backend) {
        return status != noErr ? status : kVTVideoEncoderMalfunctionErr;
    }
    backend->width = width;
    backend->height = height;

    WEXConfig config = {
        .width = width,
        .height = height,
        .bitrate = bitrate,
        .fpsNum = fpsNum,
        .fpsDen = fpsDen,
        .registryID = registryID
    };
    struct timespec deadline = deadline_after_seconds(kTransportTimeoutSeconds);
    WEXMessage reply;
    status = send_message(backend->socket, WEXOpen, &config,
                          sizeof(config), &deadline)
        ? receive_reply(backend->socket, WEXOpen, &reply, &deadline)
        : kVTVideoEncoderMalfunctionErr;
    if (status == noErr && reply.payloadBytes != 0) {
        status = kVTVideoEncoderMalfunctionErr;
    }
    if (status != noErr) {
        stop_backend(backend);
        pthread_mutex_destroy(&backend->lock);
        free(backend);
        return status;
    }

    *backend_output = backend;
    return noErr;
}

OSStatus WEXBackendFrame(WEXEncoderBackend *backend,
                         CVPixelBufferRef pixelBuffer,
                         CMTime pts, CMTime duration,
                         bool forceKeyFrame,
                         CMSampleBufferRef *encoded) {
    if (!backend || !pixelBuffer || !encoded) {
        return kVTParameterErr;
    }
    *encoded = NULL;

    if (!valid_timescale(pts) ||
        (CMTIME_IS_NUMERIC(duration) &&
         (duration.timescale <= 0 || duration.value < 0))) {
        return kVTParameterErr;
    }
    if (CVPixelBufferGetPixelFormatType(pixelBuffer) !=
            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
        CVPixelBufferGetWidth(pixelBuffer) != (size_t)backend->width ||
        CVPixelBufferGetHeight(pixelBuffer) != (size_t)backend->height ||
        !CVPixelBufferIsPlanar(pixelBuffer) ||
        CVPixelBufferGetPlaneCount(pixelBuffer) != 2 ||
        CVPixelBufferGetWidthOfPlane(pixelBuffer, 0) !=
            (size_t)backend->width ||
        CVPixelBufferGetHeightOfPlane(pixelBuffer, 0) !=
            (size_t)backend->height ||
        CVPixelBufferGetWidthOfPlane(pixelBuffer, 1) !=
            (size_t)backend->width / 2 ||
        CVPixelBufferGetHeightOfPlane(pixelBuffer, 1) !=
            (size_t)backend->height / 2) {
        return kVTVideoEncoderMalfunctionErr;
    }

    const size_t luma_bytes = (size_t)backend->width * backend->height;
    const size_t frame_bytes = luma_bytes + luma_bytes / 2;
    uint8_t *frame = malloc(frame_bytes);
    if (!frame) {
        return kVTAllocationFailedErr;
    }

    CVReturn lock_status = CVPixelBufferLockBaseAddress(
        pixelBuffer, kCVPixelBufferLock_ReadOnly);
    if (lock_status != kCVReturnSuccess) {
        free(frame);
        return kVTVideoEncoderMalfunctionErr;
    }

    uint8_t *luma = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0);
    uint8_t *chroma = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1);
    size_t luma_stride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
    size_t chroma_stride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
    bool valid_layout = luma && chroma &&
        luma_stride >= (size_t)backend->width &&
        chroma_stride >= (size_t)backend->width;
    if (valid_layout) {
        for (int32_t row = 0; row < backend->height; ++row) {
            memcpy(frame + (size_t)row * backend->width,
                   luma + (size_t)row * luma_stride,
                   (size_t)backend->width);
        }
        uint8_t *destination_chroma = frame + luma_bytes;
        for (int32_t row = 0; row < backend->height / 2; ++row) {
            memcpy(destination_chroma + (size_t)row * backend->width,
                   chroma + (size_t)row * chroma_stride,
                   (size_t)backend->width);
        }
    }
    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
    if (!valid_layout) {
        free(frame);
        return kVTVideoEncoderMalfunctionErr;
    }

    WEXFrameInfo frame_info = {
        .ptsValue = pts.value,
        .durationValue = CMTIME_IS_NUMERIC(duration) ? duration.value : 0,
        .ptsScale = pts.timescale,
        .durationScale = CMTIME_IS_NUMERIC(duration) ? duration.timescale : 0,
        .forceKeyFrame = forceKeyFrame ? 1u : 0u,
        .reserved = 0
    };
    OSStatus colorStatus = WEXPackVideoColorMetadata(pixelBuffer, &frame_info.color);
    if (colorStatus != noErr) {
        free(frame);
        return colorStatus;
    }
    uint64_t payload_bytes = sizeof(frame_info) + frame_bytes;
    if (payload_bytes > WEXMaxPacket) {
        free(frame);
        return kVTParameterErr;
    }

    uint8_t *request = malloc((size_t)payload_bytes);
    if (!request) {
        free(frame);
        return kVTAllocationFailedErr;
    }
    memcpy(request, &frame_info, sizeof(frame_info));
    memcpy(request + sizeof(frame_info), frame, frame_bytes);
    free(frame);

    pthread_mutex_lock(&backend->lock);
    if (backend->unusable || backend->socket < 0 || backend->child <= 0) {
        pthread_mutex_unlock(&backend->lock);
        free(request);
        return kVTVideoEncoderMalfunctionErr;
    }

    struct timespec deadline = deadline_after_seconds(kTransportTimeoutSeconds);
    WEXMessage reply;
    OSStatus status = send_message(backend->socket, WEXFrame, request,
                                   (uint32_t)payload_bytes, &deadline)
        ? receive_reply(backend->socket, WEXFrame, &reply, &deadline)
        : kVTVideoEncoderMalfunctionErr;
    free(request);
    if (status != noErr) {
        stop_backend(backend);
        pthread_mutex_unlock(&backend->lock);
        return status;
    }

    if (reply.payloadBytes < sizeof(WEXEncodedInfo)) {
        stop_backend(backend);
        pthread_mutex_unlock(&backend->lock);
        return kVTVideoEncoderMalfunctionErr;
    }
    uint8_t *response = malloc(reply.payloadBytes);
    if (!response) {
        stop_backend(backend);
        pthread_mutex_unlock(&backend->lock);
        return kVTAllocationFailedErr;
    }
    if (!read_exact(backend->socket, response, reply.payloadBytes, &deadline)) {
        free(response);
        stop_backend(backend);
        pthread_mutex_unlock(&backend->lock);
        return kVTVideoEncoderMalfunctionErr;
    }

    WEXEncodedInfo info;
    memcpy(&info, response, sizeof(info));
    uint64_t expected_payload = sizeof(info) + (uint64_t)info.vpsBytes +
        info.spsBytes + info.ppsBytes + info.codedBytes;
    if (info.keyFrame > 1 ||
        expected_payload != reply.payloadBytes ||
        info.ptsScale <= 0 || info.durationScale < 0 ||
        (info.durationScale == 0 && info.durationValue != 0)) {
        free(response);
        stop_backend(backend);
        pthread_mutex_unlock(&backend->lock);
        return kVTVideoEncoderMalfunctionErr;
    }

    CMSampleBufferRef sample = NULL;
    status = make_sample_buffer(&info, response + sizeof(info), &sample);
    free(response);
    if (status != noErr) {
        stop_backend(backend);
        pthread_mutex_unlock(&backend->lock);
        return status;
    }

    *encoded = sample;
    pthread_mutex_unlock(&backend->lock);
    return noErr;
}

void WEXBackendClose(WEXEncoderBackend *backend) {
    if (!backend) {
        return;
    }
    pthread_mutex_lock(&backend->lock);
    stop_backend(backend);
    pthread_mutex_unlock(&backend->lock);
    pthread_mutex_destroy(&backend->lock);
    free(backend);
}
