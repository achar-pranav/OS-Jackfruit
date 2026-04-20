/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Team Members:
 *   Pranav Hareesh Achar (PES1UG24AM196)
 *   Rahul Sharan Sharma (PES1UG24AM218)
 *
 * Implemented:
 *   - Multi-container isolation (PID, UTS, mount namespaces)
 *   - Bounded-buffer concurrent logging
 *   - Supervisor-client control IPC (Unix Sockets)
 *   - Kernel monitor integration (soft/hard limits)
 *   - Blocking 'run' command support
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int wait_fd; // Client connection for 'run' command
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

void *logging_thread(void *arg)
{
    bounded_buffer_t *buffer = (bounded_buffer_t *)arg;
    log_item_t item;

    mkdir(LOG_DIR, 0755);

    while (bounded_buffer_pop(buffer, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }
    return NULL;
}

typedef struct {
    char id[CONTAINER_ID_LEN];
    int pipe_fd;
    bounded_buffer_t *buffer;
} producer_args_t;

void *producer_thread(void *arg)
{
    producer_args_t *args = (producer_args_t *)arg;
    char chunk[LOG_CHUNK_SIZE];
    ssize_t n;

    while ((n = read(args->pipe_fd, chunk, sizeof(chunk))) > 0) {
        log_item_t item;
        memset(&item, 0, sizeof(item));
        snprintf(item.container_id, sizeof(item.container_id), "%s", args->id);
        item.length = n;
        memcpy(item.data, chunk, n);
        
        if (bounded_buffer_push(args->buffer, &item) != 0) break;
    }

    close(args->pipe_fd);
    free(args);
    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;

    // Set hostname for UTS namespace isolation
    if (sethostname(config->id, strlen(config->id)) < 0) {
        perror("sethostname");
        return 1;
    }

    // Chroot into the container rootfs
    if (chroot(config->rootfs) < 0) {
        perror("chroot");
        return 1;
    }

    // Change directory to root of the new rootfs
    if (chdir("/") < 0) {
        perror("chdir");
        return 1;
    }

    // Mount /proc for PID namespace isolation
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount proc");
        return 1;
    }

    // Redirect stdout and stderr to the logging pipe
    if (dup2(config->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(config->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        return 1;
    }
    close(config->log_write_fd);

    // Set nice value if requested
    if (config->nice_value != 0) {
        if (nice(config->nice_value) == -1 && errno != 0) {
            perror("nice");
        }
    }

    // Execute the command
    char *argv[] = {"/bin/sh", "-c", config->command, NULL};
    execv("/bin/sh", argv);

    // If execv returns, it failed
    perror("execv");
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    snprintf(req.container_id, sizeof(req.container_id), "%s", container_id);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    snprintf(req.container_id, sizeof(req.container_id), "%s", container_id);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static supervisor_ctx_t *g_ctx = NULL;

static void handle_sigchld(int sig)
{
    (void)sig;
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx) continue;

        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *curr = g_ctx->containers;
        while (curr) {
            if (curr->host_pid == pid) {
                if (g_ctx && g_ctx->monitor_fd >= 0) {
                    unregister_from_monitor(g_ctx->monitor_fd, curr->id, pid);
                }
                if (WIFEXITED(status)) {
                    curr->state = CONTAINER_EXITED;
                    curr->exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    curr->state = CONTAINER_KILLED;
                    curr->exit_signal = WTERMSIG(status);
                }

                // If a client is waiting (CMD_RUN), notify them
                if (curr->wait_fd >= 0) {
                    control_response_t res;
                    res.status = 0;
                    if (WIFEXITED(status)) {
                        snprintf(res.message, sizeof(res.message), "Container %s exited with code %d", curr->id, curr->exit_code);
                    } else {
                        snprintf(res.message, sizeof(res.message), "Container %s killed by signal %d", curr->id, curr->exit_signal);
                    }
                    send(curr->wait_fd, &res, sizeof(res), 0);
                    close(curr->wait_fd);
                    curr->wait_fd = -1;
                }
                break;
            }
            curr = curr->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

static void handle_sigterm(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}

static int run_supervisor(const char *base_rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    // 1) Set up signal handlers
    struct sigaction sa_chld, sa_term;
    sa_chld.sa_handler = handle_sigchld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    sa_term.sa_handler = handle_sigterm;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = 0;
    sigaction(SIGINT, &sa_term, NULL);
    sigaction(SIGTERM, &sa_term, NULL);

    // 2) Create the control socket and open monitor
    ctx.monitor_fd = open("/dev/" DEVICE_NAME, O_RDWR);
    if (ctx.monitor_fd < 0) {
        perror("open /dev/container_monitor (is module loaded?)");
        // We continue anyway so user-space works without kernel module
    }

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CONTROL_PATH);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto cleanup;
    }

    if (listen(ctx.server_fd, 5) < 0) {
        perror("listen");
        goto cleanup;
    }

    // 3) Spawn the logger thread (skeleton for now)
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx.log_buffer);
    if (rc != 0) {
        fprintf(stderr, "Failed to create logging thread: %s\n", strerror(rc));
        goto cleanup;
    }

    fprintf(stderr, "Supervisor started. Base rootfs: %s\n", base_rootfs);

    // 4) Main event loop
    while (!ctx.should_stop) {
        struct timeval tv = {1, 0}; // 1 second timeout
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ctx.server_fd, &fds);

        int max_fd = ctx.server_fd;

        int sel = select(max_fd + 1, &fds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (sel == 0) continue; // Timeout

        if (FD_ISSET(ctx.server_fd, &fds)) {
            int client_fd = accept(ctx.server_fd, NULL, NULL);
            if (client_fd < 0) {
                perror("accept");
                continue;
            }

            control_request_t req;
            ssize_t n = recv(client_fd, &req, sizeof(req), 0);
            if (n <= 0) {
                if (n < 0) perror("recv");
                close(client_fd);
                continue;
            }

            control_response_t res = {0, ""};
            int should_close_client = 1;

            if (req.kind == CMD_START || req.kind == CMD_RUN) {
                // Handle container start
                pthread_mutex_lock(&ctx.metadata_lock);
                
                container_record_t *new_rec = malloc(sizeof(container_record_t));
                snprintf(new_rec->id, sizeof(new_rec->id), "%s", req.container_id);
                new_rec->state = CONTAINER_STARTING;
                new_rec->started_at = time(NULL);
                new_rec->soft_limit_bytes = req.soft_limit_bytes;
                new_rec->hard_limit_bytes = req.hard_limit_bytes;
                new_rec->wait_fd = -1;
                new_rec->next = ctx.containers;
                ctx.containers = new_rec;
                
                if (req.kind == CMD_RUN) {
                    new_rec->wait_fd = client_fd;
                    should_close_client = 0;
                }

                child_config_t *c_config = malloc(sizeof(child_config_t));
                snprintf(c_config->id, sizeof(c_config->id), "%s", req.container_id);
                snprintf(c_config->rootfs, sizeof(c_config->rootfs), "%s", req.rootfs);
                snprintf(c_config->command, sizeof(c_config->command), "%s", req.command);
                c_config->nice_value = req.nice_value;
                
                // For Task 1, we can just use a dummy pipe or stdout for now
                // but let's at least create a pipe to not crash child_fn
                int pipefds[2];
                if (pipe(pipefds) < 0) {
                    perror("pipe");
                    res.status = -1;
                    strcpy(res.message, "Failed to create logging pipe");
                } else {
                    c_config->log_write_fd = pipefds[1];
                    // We should handle the read end (pipefds[0]) in Task 3
                    
                    void *stack = malloc(STACK_SIZE);
                    pid_t child_pid = clone(child_fn, stack + STACK_SIZE,
                                            CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                                            c_config);
                    
                    if (child_pid < 0) {
                        perror("clone");
                        res.status = -1;
                        strcpy(res.message, "Failed to clone container (root privileges required for namespaces)");
                        new_rec->state = CONTAINER_STOPPED; // Mark as failed/stopped
                        close(pipefds[0]);
                        close(pipefds[1]);
                    } else {
                        new_rec->host_pid = child_pid;
                        new_rec->state = CONTAINER_RUNNING;
                        res.status = 0;
                        sprintf(res.message, "Container %s started with PID %d", req.container_id, child_pid);
                        close(pipefds[1]); // Close write end in parent

                        if (ctx.monitor_fd >= 0) {
                            register_with_monitor(ctx.monitor_fd, req.container_id, child_pid,
                                               req.soft_limit_bytes, req.hard_limit_bytes);
                        }

                        // Start producer thread to read from pipefds[0]
                        producer_args_t *p_args = malloc(sizeof(producer_args_t));
                        snprintf(p_args->id, sizeof(p_args->id), "%s", req.container_id);
                        p_args->pipe_fd = pipefds[0];
                        p_args->buffer = &ctx.log_buffer;
                        pthread_t pt;
                        pthread_create(&pt, NULL, producer_thread, p_args);
                        pthread_detach(pt);
                    }
                }
                
                pthread_mutex_unlock(&ctx.metadata_lock);
            } else if (req.kind == CMD_STOP) {
                pthread_mutex_lock(&ctx.metadata_lock);
                container_record_t *curr = ctx.containers;
                while (curr) {
                    if (strcmp(curr->id, req.container_id) == 0) {
                        if (curr->state == CONTAINER_RUNNING) {
                            kill(curr->host_pid, SIGTERM);
                            curr->state = CONTAINER_STOPPED;
                            res.status = 0;
                            strcpy(res.message, "Stop signal sent");
                        } else {
                            res.status = -1;
                            strcpy(res.message, "Container not running");
                        }
                        break;
                    }
                    curr = curr->next;
                }
                if (!curr) {
                    res.status = -1;
                    strcpy(res.message, "Container not found");
                }
                pthread_mutex_unlock(&ctx.metadata_lock);
            } else if (req.kind == CMD_LOGS) {
                // Return log path
                res.status = 0;
                snprintf(res.message, CONTROL_MESSAGE_LEN, "logs/%s.log", req.container_id);
            } else if (req.kind == CMD_PS) {
                res.status = 0;
                pthread_mutex_lock(&ctx.metadata_lock);
                container_record_t *curr = ctx.containers;
                char *msg_ptr = res.message;
                msg_ptr += sprintf(msg_ptr, "%-10s %-10s %-10s\n", "ID", "PID", "STATE");
                while (curr && (msg_ptr - res.message < CONTROL_MESSAGE_LEN - 40)) {
                    msg_ptr += sprintf(msg_ptr, "%-10s %-10d %-10s\n", curr->id, curr->host_pid, state_to_string(curr->state));
                    curr = curr->next;
                }
                pthread_mutex_unlock(&ctx.metadata_lock);
            } else {
                res.status = -1;
                strcpy(res.message, "Command not implemented");
            }

            if (should_close_client) {
                send(client_fd, &res, sizeof(res), 0);
                close(client_fd);
            }
        }
    }

cleanup:
    if (ctx.server_fd >= 0) close(ctx.server_fd);
    unlink(CONTROL_PATH);
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    // pthread_join(ctx.logger_thread, NULL); // Skip join for now as thread is empty
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CONTROL_PATH);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect to supervisor (is it running?)");
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), 0) < 0) {
        perror("send");
        close(fd);
        return 1;
    }

    control_response_t res;
    if (recv(fd, &res, sizeof(res), 0) < 0) {
        perror("recv");
        close(fd);
        return 1;
    }

    if (res.status == 0) {
        printf("%s\n", res.message);
    } else {
        fprintf(stderr, "Error: %s\n", res.message);
    }

    close(fd);
    return res.status == 0 ? 0 : 1;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    snprintf(req.container_id, sizeof(req.container_id), "%s", argv[2]);
    snprintf(req.rootfs, sizeof(req.rootfs), "%s", argv[3]);
    snprintf(req.command, sizeof(req.command), "%s", argv[4]);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    snprintf(req.container_id, sizeof(req.container_id), "%s", argv[2]);
    snprintf(req.rootfs, sizeof(req.rootfs), "%s", argv[3]);
    snprintf(req.command, sizeof(req.command), "%s", argv[4]);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    printf("Expected states include: %s, %s, %s, %s, %s\n",
           state_to_string(CONTAINER_STARTING),
           state_to_string(CONTAINER_RUNNING),
           state_to_string(CONTAINER_STOPPED),
           state_to_string(CONTAINER_KILLED),
           state_to_string(CONTAINER_EXITED));
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    snprintf(req.container_id, sizeof(req.container_id), "%s", argv[2]);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CONTROL_PATH);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return 1;
    }

    send(fd, &req, sizeof(req), 0);
    control_response_t res;
    recv(fd, &res, sizeof(res), 0);
    close(fd);

    if (res.status == 0) {
        FILE *f = fopen(res.message, "r");
        if (!f) {
            perror("fopen log file");
            return 1;
        }
        char buf[1024];
        while (fgets(buf, sizeof(buf), f)) {
            printf("%s", buf);
        }
        fclose(f);
        return 0;
    } else {
        fprintf(stderr, "Error: %s\n", res.message);
        return 1;
    }
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    snprintf(req.container_id, sizeof(req.container_id), "%s", argv[2]);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
