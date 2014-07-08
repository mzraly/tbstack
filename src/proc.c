/*
 * tbstack -- fast stack trace utility
 *
 * Copyright (c) 2014, Tbricks AB
 * All rights reserved.
 */

#include "mem_map.h"
#include "proc.h"

#include <ctype.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <limits.h>
#include <linux/limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_process_vm_readv
#define SYS_process_vm_readv 310
#endif

#define SLEEP_WAIT 500

int attached_pid = 0;
int attached_tid = 0;
int stopped_pid = 0;

/* timeout on waiting for process to stop (us) */
extern int stop_timeout;
static int sleep_time = 0;

int sleep_count = 0;
size_t total_length = 0;

extern struct timeval freeze_time;
extern struct timeval unfreeze_time;

int proc_stopped(int pid)
{
    FILE *f;
    char buf[128];
    char c;
    int rc = -1;

    sprintf(buf, "/proc/%d/status", pid);
    if ((f = fopen(buf, "r")) == NULL) {
        fprintf(stderr, "cannot open %s: %s\n", buf, strerror(errno));
        return -1;
    }

    while (fgets(buf, sizeof(buf), f)) {
        if (sscanf(buf, "State:\t%c", &c) == 1) {
            rc = (c == 't' || c == 'T');
            break;
        }
    }

    fclose(f);
    return rc;
}

struct mem_map *create_maps(int pid)
{
    FILE *f;
    char buf[PATH_MAX+128];

    size_t addr_start, addr_end, offset, len;
    char r, w, x, p;
    int dev_major, dev_minor, inode;
    char path[PATH_MAX];
    int scan;

    struct mem_map *map;
    struct mem_region *region;

    sprintf(buf, "/proc/%d/maps", pid);
    if ((f = fopen(buf, "r")) == NULL) {
        fprintf(stderr, "cannot open %s: %s\n", buf, strerror(errno));
        return NULL;
    }

    map = malloc(sizeof(struct mem_map));
    mem_map_init(map);

    while (fgets(buf, sizeof(buf), f)) {
        scan = sscanf(buf, "%lx-%lx %c%c%c%c %lx %x:%x %d %[^\t\n]",
                &addr_start, &addr_end,
                &r, &w, &x, &p,
                &offset,
                &dev_major, &dev_minor,
                &inode,
                path);

        if (scan < 10) {
            fprintf(stderr, "warning: unable to parse maps entry '%s'\n", buf);
            continue;
        }

        region = malloc(sizeof(struct mem_region));
        mem_region_init(region);

        region->start = (void *)addr_start;
        region->length = addr_end - addr_start;
        region->offset = offset;
        if (scan > 10 && path[0] != '\0') {
            if (!strcmp(path, "[vdso]")) {
                region->type = MEM_REGION_TYPE_VDSO;
            } else if (!strcmp(path, "[vsyscall]")) {
                region->type = MEM_REGION_TYPE_VSYSCALL;
            } else if ((len = strlen(path)) > 10 &&
                    !strcmp(path + len - 10, " (deleted)")) {
                *(path + len - 10) = '\0';
                region->path = strdup(path);
                region->type = MEM_REGION_TYPE_DELETED;
            } else {
                region->path = strdup(path);
                region->type = MEM_REGION_TYPE_MMAP;
            }
        }

        if (mem_map_add_region(map, region) != 0) {
            mem_map_destroy(map);
            map = NULL;
            break;
        }
    }

    if (map != NULL)
        mem_map_create_region_index(map);

    fclose(f);
    return map;
}

int print_proc_maps(int pid)
{
    char cmd[32];
    sprintf(cmd, "cat /proc/%d/maps 1>&2", pid);
    return system(cmd);
}

static int dir_select(const struct dirent *entry)
{
    const char *c = entry->d_name;
    while (*c)
        if (!isdigit(*c++))
            return 0;
    return 1;
}

int get_threads(int pid, int **tids)
{
    char buf[32];
    struct dirent **namelist;
    int cur, i, n;

    snprintf(buf, sizeof(buf), "/proc/%d/task", pid);

    n = scandir(buf, &namelist, dir_select, NULL);
    if (n < 0) {
        perror(buf);
        return -1;
    } else {
        *tids = malloc(sizeof(int)*n);
        i = 0;
        while (i < n) {
            cur = atoi(namelist[i]->d_name);
            (*tids)[i] = cur;
            free(namelist[i++]);
        }
        free(namelist);
    }

    return n;
}

int attach_process(int pid)
{
    int status = 0;

    gettimeofday(&freeze_time, NULL);

    attached_pid = pid;
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) < 0) {
        perror("attach");
        detach_process(pid);
        return -1;
    }
    if (!proc_stopped(pid)) {
        if (waitpid(pid, &status, WUNTRACED) < 0) {
            fprintf(stderr, "waitpid %d: %s\n", pid, strerror(errno));
            detach_process(pid);
            return -1;
        }
        if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP)
            fprintf(stderr, "warning: waitpid(%d) WIFSTOPPED=%d WSTOPSIG=%d\n",
                    pid, WIFSTOPPED(status), WSTOPSIG(status));
    }
    stopped_pid = pid;
    if (kill(pid, SIGSTOP) < 0) {
        perror("send SIGSTOP");
        return -1;
    }
    return 0;
}

int attach_thread(int tid)
{
    attached_tid = tid;
    if (ptrace(PTRACE_ATTACH, tid, NULL, NULL) < 0) {
        perror("PTRACE_ATTACH");
        return -1;
    }
    if (wait_thread(tid) < 0)
        return -1;
    return 0;
}

int detach_process(int pid)
{
    int rc = 0;
    if (ptrace(PTRACE_DETACH, pid, NULL, NULL) < 0) {
        perror("detach");
        rc = -1;
    }
    if (kill(pid, SIGCONT) < 0) {
        perror("send SIGCONT");
        rc = -1;
    }

    attached_pid = 0;
    gettimeofday(&unfreeze_time, NULL);
    return rc;
}

int detach_thread(int tid)
{
    long rc = ptrace(PTRACE_DETACH, tid, NULL, NULL);
    attached_tid = 0;
    if (rc < 0) {
        perror("PTRACE_DETACH");
        return -1;
    }
    return 0;
}

int wait_thread(int tid)
{
    int rc;
    while (!(rc = proc_stopped(tid))) {
        if (stop_timeout && sleep_time > stop_timeout) {
            fprintf(stderr, "timeout waiting for thread %d to stop", tid);
            return -1;
        }
        usleep(SLEEP_WAIT);
        sleep_time += SLEEP_WAIT;
        sleep_count++;
    }
    return (rc == -1 ? -1 : 0);
}

int copy_memory_process_vm_readv(int pid,
        struct mem_data_chunk **frames, int n_frames)
{
    struct iovec *local_iov, *remote_iov;
    ssize_t *frame_bytes;
    int i, rc = -1;
    ssize_t bytes_total = 0;
    int seg_count = 0;

    local_iov = malloc(sizeof(struct iovec)*n_frames);
    remote_iov = malloc(sizeof(struct iovec)*n_frames);
    frame_bytes = malloc(sizeof(ssize_t)*n_frames);

    for (i = 0; i < n_frames; ++i) {
        local_iov[i].iov_base = frames[i]->data;
        local_iov[i].iov_len = frames[i]->length;
        remote_iov[i].iov_base = frames[i]->start;
        remote_iov[i].iov_len = frames[i]->length;

        bytes_total += frames[i]->length;
        frame_bytes[i] = bytes_total;
    }

    bytes_total = 0;
    while (1) {
        ssize_t bytes_read;
        int frames_to_read = n_frames - seg_count;
        if (frames_to_read > IOV_MAX)
            frames_to_read = IOV_MAX;

        bytes_read = syscall(SYS_process_vm_readv,
                pid,
                local_iov + seg_count,
                frames_to_read,
                remote_iov + seg_count,
                frames_to_read,
                0ULL);

        if (bytes_read < 0) {
            perror("process_vm_readv");
            goto process_vm_readv_end;
        }

        bytes_total += bytes_read;
        total_length = bytes_total;
        for (seg_count = n_frames-1; seg_count >= 0; --seg_count) {
            if (frame_bytes[seg_count] == bytes_total)
                break;
        }

        if (seg_count < 0) {
            fprintf(stderr, "unknown number of bytes returned by "
                    "process_vm_readv: bytes_read=%ld "
                    "bytes_total=%ld seg_count=%d\n",
                    bytes_read, bytes_total, seg_count);
            goto process_vm_readv_end;
        }

        if (seg_count == (n_frames-1))
            break;

        ++seg_count;
    }

    rc = 0;

process_vm_readv_end:
    free(local_iov);
    free(remote_iov);
    free(frame_bytes);
    return rc;
}

int copy_memory_proc_mem(int pid, struct mem_data_chunk **frames, int n_frames)
{
    int i = 0;
    char fname[32];
    FILE *f;
    int rc = -1;

    sprintf(fname, "/proc/%d/mem", pid);
    if ((f = fopen(fname, "r")) == NULL) {
        fprintf(stderr, "cannot open %s\n", fname);
        perror(fname);
        return -1;
    }

    for (i = 0; i < n_frames; ++i) {
        if (fseek(f, (long)frames[i]->start, SEEK_SET) < 0) {
            fprintf(stderr, "seek at %s:0x%lx (#%d) failed\n",
                    fname,
                    (long)frames[i]->start,
                    i);
            perror(fname);
            goto proc_mem_end;
        }
        if (fread(frames[i]->data, frames[i]->length, 1, f) != 1) {
            fprintf(stderr, "read at %s:0x%lx (#%d) failed\n",
                    fname,
                    (long)frames[i]->start,
                    i);
            perror(fname);
            goto proc_mem_end;
        }
        total_length += frames[i]->length;
    }

    rc = 0;

proc_mem_end:
    fclose(f);
    return rc;
}

void *get_vdso()
{
    static const char *auxv = "/proc/self/auxv";
    FILE *f;
    long entry[2];

    f = fopen(auxv, "r");
    if (f == NULL) {
        perror(auxv);
        return NULL;
    }

    while (!feof(f)) {
        if (fread(entry, sizeof(entry), 1, f) != 1)
            goto get_vdso_fail;

        if (entry[0] == AT_SYSINFO_EHDR) {
            fclose(f);
            return (void *)entry[1];
        }
    }

get_vdso_fail:
    perror(auxv);
    fclose(f);
    return NULL;
}

void quit_handler(int signum)
{
    if (attached_tid)
        ptrace(PTRACE_DETACH, attached_tid, NULL, NULL);
    if (attached_pid)
        ptrace(PTRACE_DETACH, attached_pid, NULL, NULL);
    if (stopped_pid)
        kill(stopped_pid, SIGCONT);
    if (signum == SIGSEGV) {
        static volatile int *n = NULL;
        *n = 1969;
    }
    exit(1);
}