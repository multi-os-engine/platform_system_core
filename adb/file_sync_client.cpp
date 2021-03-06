/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include <memory>

#include "sysdeps.h"

#include "adb.h"
#include "adb_client.h"
#include "adb_io.h"
#include "adb_utils.h"
#include "file_sync_service.h"

#include <base/file.h>
#include <base/strings.h>
#include <base/stringprintf.h>

struct syncsendbuf {
    unsigned id;
    unsigned size;
    char data[SYNC_DATA_MAX];
};

static long long NOW() {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return ((long long) tv.tv_usec) + 1000000LL * ((long long) tv.tv_sec);
}

static void print_transfer_progress(uint64_t bytes_current,
                                    uint64_t bytes_total) {
    if (bytes_total == 0) return;

    fprintf(stderr, "\rTransferring: %" PRIu64 "/%" PRIu64 " (%d%%)",
            bytes_current, bytes_total,
            (int) (bytes_current * 100 / bytes_total));

    if (bytes_current == bytes_total) {
        fputc('\n', stderr);
    }

    fflush(stderr);
}

class SyncConnection {
  public:
    SyncConnection() : total_bytes(0), start_time_(NOW()) {
        max = SYNC_DATA_MAX; // TODO: decide at runtime.

        std::string error;
        fd = adb_connect("sync:", &error);
        if (fd < 0) {
            fprintf(stderr, "error: %s\n", error.c_str());
        }
    }

    ~SyncConnection() {
        if (!IsValid()) return;

        SendQuit();
        ShowTransferRate();
        adb_close(fd);
    }

    bool IsValid() { return fd >= 0; }

    bool SendRequest(int id, const char* path_and_mode) {
        size_t path_length = strlen(path_and_mode);
        if (path_length > 1024) {
            fprintf(stderr, "SendRequest failed: path too long: %zu", path_length);
            errno = ENAMETOOLONG;
            return false;
        }

        // Sending header and payload in a single write makes a noticeable
        // difference to "adb sync" performance.
        char buf[sizeof(SyncRequest) + path_length];
        SyncRequest* req = reinterpret_cast<SyncRequest*>(buf);
        req->id = id;
        req->path_length = path_length;
        char* data = reinterpret_cast<char*>(req + 1);
        memcpy(data, path_and_mode, path_length);

        return WriteFdExactly(fd, buf, sizeof(buf));
    }

    // Sending header, payload, and footer in a single write makes a huge
    // difference to "adb sync" performance.
    bool SendSmallFile(const char* path_and_mode,
                       const char* data, size_t data_length,
                       unsigned mtime) {
        size_t path_length = strlen(path_and_mode);
        if (path_length > 1024) {
            fprintf(stderr, "SendSmallFile failed: path too long: %zu", path_length);
            errno = ENAMETOOLONG;
            return false;
        }

        char buf[sizeof(SyncRequest) + path_length +
                 sizeof(SyncRequest) + data_length +
                 sizeof(SyncRequest)];
        char* p = buf;

        SyncRequest* req_send = reinterpret_cast<SyncRequest*>(p);
        req_send->id = ID_SEND;
        req_send->path_length = path_length;
        p += sizeof(SyncRequest);
        memcpy(p, path_and_mode, path_length);
        p += path_length;

        SyncRequest* req_data = reinterpret_cast<SyncRequest*>(p);
        req_data->id = ID_DATA;
        req_data->path_length = data_length;
        p += sizeof(SyncRequest);
        memcpy(p, data, data_length);
        p += data_length;

        SyncRequest* req_done = reinterpret_cast<SyncRequest*>(p);
        req_done->id = ID_DONE;
        req_done->path_length = mtime;
        p += sizeof(SyncRequest);

        if (!WriteFdExactly(fd, buf, (p-buf))) return false;

        total_bytes += data_length;
        return true;
    }

    bool CopyDone(const char* from, const char* to) {
        syncmsg msg;
        if (!ReadFdExactly(fd, &msg.status, sizeof(msg.status))) {
            fprintf(stderr, "failed to copy '%s' to '%s': no ID_DONE: %s\n",
                    from, to, strerror(errno));
            return false;
        }
        if (msg.status.id == ID_OKAY) {
            return true;
        }
        if (msg.status.id != ID_FAIL) {
            fprintf(stderr, "failed to copy '%s' to '%s': unknown reason\n", from, to);
            return false;
        }
        char buffer[msg.status.msglen + 1];
        if (!ReadFdExactly(fd, buffer, msg.status.msglen)) {
            fprintf(stderr, "failed to copy '%s' to '%s'; failed to read reason (!): %s\n",
                    from, to, strerror(errno));
            return false;
        }
        buffer[msg.status.msglen] = 0;
        fprintf(stderr, "failed to copy '%s' to '%s': %s\n", from, to, buffer);
        return false;
    }

    uint64_t total_bytes;

    // TODO: add a char[max] buffer here, to replace syncsendbuf...
    int fd;
    size_t max;

  private:
    uint64_t start_time_;

    void SendQuit() {
        SendRequest(ID_QUIT, ""); // TODO: add a SendResponse?
    }

    void ShowTransferRate() {
        uint64_t t = NOW() - start_time_;
        if (total_bytes == 0 || t == 0) return;

        fprintf(stderr, "%lld KB/s (%" PRId64 " bytes in %lld.%03llds)\n",
                ((total_bytes * 1000000LL) / t) / 1024LL,
                total_bytes, (t / 1000000LL), (t % 1000000LL) / 1000LL);
    }
};

typedef void (*sync_ls_cb)(unsigned mode, unsigned size, unsigned time, const char* name, void* cookie);

static bool sync_ls(SyncConnection& sc, const char* path, sync_ls_cb func, void* cookie) {
    if (!sc.SendRequest(ID_LIST, path)) return false;

    while (true) {
        syncmsg msg;
        if (!ReadFdExactly(sc.fd, &msg.dent, sizeof(msg.dent))) return false;

        if (msg.dent.id == ID_DONE) return true;
        if (msg.dent.id != ID_DENT) return false;

        size_t len = msg.dent.namelen;
        if (len > 256) return false; // TODO: resize buffer? continue?

        char buf[257];
        if (!ReadFdExactly(sc.fd, buf, len)) return false;
        buf[len] = 0;

        func(msg.dent.mode, msg.dent.size, msg.dent.time, buf, cookie);
    }
}

static bool sync_finish_stat(SyncConnection& sc, unsigned int* timestamp,
                             unsigned int* mode, unsigned int* size) {
    syncmsg msg;
    if (!ReadFdExactly(sc.fd, &msg.stat, sizeof(msg.stat)) || msg.stat.id != ID_STAT) {
        return false;
    }

    if (timestamp) *timestamp = msg.stat.time;
    if (mode) *mode = msg.stat.mode;
    if (size) *size = msg.stat.size;

    return true;
}

static bool sync_stat(SyncConnection& sc, const char* path,
                      unsigned int* timestamp, unsigned int* mode, unsigned int* size) {
    return sc.SendRequest(ID_STAT, path) && sync_finish_stat(sc, timestamp, mode, size);
}

static bool SendLargeFile(SyncConnection& sc, const char* path_and_mode, const char* path,
                          unsigned mtime, bool show_progress) {
    if (!sc.SendRequest(ID_SEND, path_and_mode)) {
        fprintf(stderr, "failed to send ID_SEND message '%s': %s\n",
                path_and_mode, strerror(errno));
        return false;
    }

    unsigned long long size = 0;
    if (show_progress) {
        // Determine local file size.
        struct stat st;
        if (stat(path, &st) == -1) {
            fprintf(stderr, "cannot stat '%s': %s\n", path, strerror(errno));
            return false;
        }

        size = st.st_size;
    }

    int lfd = adb_open(path, O_RDONLY);
    if (lfd < 0) {
        fprintf(stderr, "cannot open '%s': %s\n", path, strerror(errno));
        return false;
    }

    syncsendbuf sbuf;
    sbuf.id = ID_DATA;
    while (true) {
        int ret = adb_read(lfd, sbuf.data, sc.max);
        if (ret <= 0) {
            if (ret < 0) {
                fprintf(stderr, "cannot read '%s': %s\n", path, strerror(errno));
                adb_close(lfd);
                return false;
            }
            break;
        }

        sbuf.size = ret;
        if (!WriteFdExactly(sc.fd, &sbuf, sizeof(unsigned) * 2 + ret)) {
            adb_close(lfd);
            return false;
        }
        sc.total_bytes += ret;

        if (show_progress) {
            print_transfer_progress(sc.total_bytes, size);
        }
    }

    adb_close(lfd);

    syncmsg msg;
    msg.data.id = ID_DONE;
    msg.data.size = mtime;
    if (!WriteFdExactly(sc.fd, &msg.data, sizeof(msg.data))) {
        fprintf(stderr, "failed to send ID_DONE message for '%s': %s\n", path, strerror(errno));
        return false;
    }

    return true;
}

static bool sync_send(SyncConnection& sc, const char* lpath, const char* rpath,
                      unsigned mtime, mode_t mode, bool show_progress)
{
    std::string path_and_mode = android::base::StringPrintf("%s,%d", rpath, mode);

    if (S_ISLNK(mode)) {
#if !defined(_WIN32)
        char buf[PATH_MAX];
        ssize_t data_length = readlink(lpath, buf, PATH_MAX - 1);
        if (data_length == -1) {
            fprintf(stderr, "readlink '%s' failed: %s\n", lpath, strerror(errno));
            return false;
        }
        buf[data_length++] = '\0';

        if (!sc.SendSmallFile(path_and_mode.c_str(), buf, data_length, mtime)) return false;
        return sc.CopyDone(lpath, rpath);
#endif
    }

    if (!S_ISREG(mode)) {
        fprintf(stderr, "local file '%s' has unsupported mode: 0o%o\n", lpath, mode);
        return false;
    }

    struct stat st;
    if (stat(lpath, &st) == -1) {
        fprintf(stderr, "stat '%s' failed: %s\n", lpath, strerror(errno));
        return false;
    }
    if (st.st_size < SYNC_DATA_MAX) {
        std::string data;
        if (!android::base::ReadFileToString(lpath, &data)) {
            fprintf(stderr, "failed to read all of '%s': %s\n", lpath, strerror(errno));
            return false;
        }
        if (!sc.SendSmallFile(path_and_mode.c_str(), data.data(), data.size(), mtime)) return false;
    } else {
        if (!SendLargeFile(sc, path_and_mode.c_str(), lpath, mtime, show_progress)) return false;
    }
    return sc.CopyDone(lpath, rpath);
}

static bool sync_recv(SyncConnection& sc, const char* rpath, const char* lpath, bool show_progress) {
    syncmsg msg;
    int lfd = -1;

    size_t len = strlen(rpath);
    if (len > 1024) return false;

    unsigned size = 0;
    if (show_progress) {
        if (!sync_stat(sc, rpath, nullptr, nullptr, &size)) return false;
    }

    if (!sc.SendRequest(ID_RECV, rpath)) return false;
    if (!ReadFdExactly(sc.fd, &msg.data, sizeof(msg.data))) return false;

    unsigned id = msg.data.id;

    if (id == ID_DATA || id == ID_DONE) {
        adb_unlink(lpath);
        mkdirs(lpath);
        lfd = adb_creat(lpath, 0644);
        if(lfd < 0) {
            fprintf(stderr, "cannot create '%s': %s\n", lpath, strerror(errno));
            return false;
        }
        goto handle_data;
    } else {
        goto remote_error;
    }

    while (true) {
        char buffer[SYNC_DATA_MAX];

        if (!ReadFdExactly(sc.fd, &msg.data, sizeof(msg.data))) {
            adb_close(lfd);
            return false;
        }
        id = msg.data.id;

    handle_data:
        len = msg.data.size;
        if (id == ID_DONE) break;
        if (id != ID_DATA) goto remote_error;
        if (len > sc.max) {
            fprintf(stderr, "msg.data.size too large: %zu (max %zu)\n", len, sc.max);
            adb_close(lfd);
            return false;
        }

        if (!ReadFdExactly(sc.fd, buffer, len)) {
            adb_close(lfd);
            return false;
        }

        if (!WriteFdExactly(lfd, buffer, len)) {
            fprintf(stderr, "cannot write '%s': %s\n", rpath, strerror(errno));
            adb_close(lfd);
            return false;
        }

        sc.total_bytes += len;

        if (show_progress) {
            print_transfer_progress(sc.total_bytes, size);
        }
    }

    adb_close(lfd);
    return true;

remote_error:
    adb_close(lfd);
    adb_unlink(lpath);
    sc.CopyDone(rpath, lpath);
    return false;
}

static void do_sync_ls_cb(unsigned mode, unsigned size, unsigned time,
                          const char* name, void* /*cookie*/) {
    printf("%08x %08x %08x %s\n", mode, size, time, name);
}

bool do_sync_ls(const char* path) {
    SyncConnection sc;
    if (!sc.IsValid()) return false;

    return sync_ls(sc, path, do_sync_ls_cb, 0);
}

struct copyinfo
{
    copyinfo *next;
    const char *src;
    const char *dst;
    unsigned int time;
    unsigned int mode;
    uint64_t size;
    int flag;
};

static copyinfo* mkcopyinfo(const char* spath, const char* dpath, const char* name, int isdir) {
    int slen = strlen(spath);
    int dlen = strlen(dpath);
    int nlen = strlen(name);
    int ssize = slen + nlen + 2;
    int dsize = dlen + nlen + 2;

    copyinfo *ci = reinterpret_cast<copyinfo*>(malloc(sizeof(copyinfo) + ssize + dsize));
    if(ci == 0) {
        fprintf(stderr, "out of memory\n");
        abort();
    }

    ci->next = 0;
    ci->time = 0;
    ci->mode = 0;
    ci->size = 0;
    ci->flag = 0;
    ci->src = (const char*)(ci + 1);
    ci->dst = ci->src + ssize;
    snprintf((char*) ci->src, ssize, isdir ? "%s%s/" : "%s%s", spath, name);
    snprintf((char*) ci->dst, dsize, isdir ? "%s%s/" : "%s%s", dpath, name);

    return ci;
}

static bool IsDotOrDotDot(const char* name) {
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

static int local_build_list(copyinfo** filelist, const char* lpath, const char* rpath) {
    copyinfo *dirlist = 0;
    copyinfo *ci, *next;

    std::unique_ptr<DIR, int(*)(DIR*)> dir(opendir(lpath), closedir);
    if (!dir) {
        fprintf(stderr, "cannot open '%s': %s\n", lpath, strerror(errno));
        return -1;
    }

    dirent *de;
    while ((de = readdir(dir.get()))) {
        if (IsDotOrDotDot(de->d_name)) continue;

        char stat_path[PATH_MAX];
        if (strlen(lpath) + strlen(de->d_name) + 1 > sizeof(stat_path)) {
            fprintf(stderr, "skipping long path '%s%s'\n", lpath, de->d_name);
            continue;
        }
        strcpy(stat_path, lpath);
        strcat(stat_path, de->d_name);

        struct stat st;
        if (!lstat(stat_path, &st)) {
            if (S_ISDIR(st.st_mode)) {
                ci = mkcopyinfo(lpath, rpath, de->d_name, 1);
                ci->next = dirlist;
                dirlist = ci;
            } else {
                ci = mkcopyinfo(lpath, rpath, de->d_name, 0);
                if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
                    fprintf(stderr, "skipping special file '%s'\n", ci->src);
                    free(ci);
                } else {
                    ci->time = st.st_mtime;
                    ci->mode = st.st_mode;
                    ci->size = st.st_size;
                    ci->next = *filelist;
                    *filelist = ci;
                }
            }
        } else {
            fprintf(stderr, "cannot lstat '%s': %s\n",stat_path , strerror(errno));
        }
    }

    // Close this directory and recurse.
    dir.reset();
    for (ci = dirlist; ci != 0; ci = next) {
        next = ci->next;
        local_build_list(filelist, ci->src, ci->dst);
        free(ci);
    }

    return 0;
}

static bool copy_local_dir_remote(SyncConnection& sc, const char* lpath, const char* rpath,
                                  bool check_timestamps, bool list_only) {
    copyinfo *filelist = 0;
    copyinfo *ci, *next;
    int pushed = 0;
    int skipped = 0;

    if ((lpath[0] == 0) || (rpath[0] == 0)) return false;
    if (lpath[strlen(lpath) - 1] != '/') {
        int  tmplen = strlen(lpath)+2;
        char *tmp = reinterpret_cast<char*>(malloc(tmplen));
        if(tmp == 0) return false;
        snprintf(tmp, tmplen, "%s/",lpath);
        lpath = tmp;
    }
    if (rpath[strlen(rpath) - 1] != '/') {
        int tmplen = strlen(rpath)+2;
        char *tmp = reinterpret_cast<char*>(malloc(tmplen));
        if(tmp == 0) return false;
        snprintf(tmp, tmplen, "%s/",rpath);
        rpath = tmp;
    }

    if (local_build_list(&filelist, lpath, rpath)) {
        return false;
    }

    if (check_timestamps) {
        for (ci = filelist; ci != 0; ci = ci->next) {
            if (!sc.SendRequest(ID_STAT, ci->dst)) return false;
        }
        for(ci = filelist; ci != 0; ci = ci->next) {
            unsigned int timestamp, mode, size;
            if (!sync_finish_stat(sc, &timestamp, &mode, &size)) return false;
            if (size == ci->size) {
                /* for links, we cannot update the atime/mtime */
                if ((S_ISREG(ci->mode & mode) && timestamp == ci->time) ||
                        (S_ISLNK(ci->mode & mode) && timestamp >= ci->time)) {
                    ci->flag = 1;
                }
            }
        }
    }
    for (ci = filelist; ci != 0; ci = next) {
        next = ci->next;
        if (ci->flag == 0) {
            fprintf(stderr, "%spush: %s -> %s\n", list_only ? "would " : "", ci->src, ci->dst);
            if (!list_only && !sync_send(sc, ci->src, ci->dst, ci->time, ci->mode, false)) {
                return false;
            }
            pushed++;
        } else {
            skipped++;
        }
        free(ci);
    }

    fprintf(stderr, "%d file%s pushed. %d file%s skipped.\n",
            pushed, (pushed == 1) ? "" : "s",
            skipped, (skipped == 1) ? "" : "s");

    return true;
}

bool do_sync_push(const char* lpath, const char* rpath, bool show_progress) {
    SyncConnection sc;
    if (!sc.IsValid()) return false;

    struct stat st;
    if (stat(lpath, &st)) {
        fprintf(stderr, "cannot stat '%s': %s\n", lpath, strerror(errno));
        return false;
    }

    if (S_ISDIR(st.st_mode)) {
        return copy_local_dir_remote(sc, lpath, rpath, false, false);
    }

    unsigned mode;
    if (!sync_stat(sc, rpath, nullptr, &mode, nullptr)) return false;
    std::string path_holder;
    if (mode != 0 && S_ISDIR(mode)) {
        // If we're copying a local file to a remote directory,
        // we really want to copy to remote_dir + "/" + local_filename.
        path_holder = android::base::StringPrintf("%s/%s", rpath, adb_basename(lpath).c_str());
        rpath = path_holder.c_str();
    }
    return sync_send(sc, lpath, rpath, st.st_mtime, st.st_mode, show_progress);
}


struct sync_ls_build_list_cb_args {
    copyinfo **filelist;
    copyinfo **dirlist;
    const char *rpath;
    const char *lpath;
};

static void sync_ls_build_list_cb(unsigned mode, unsigned size, unsigned time,
                                  const char* name, void* cookie)
{
    sync_ls_build_list_cb_args *args = (sync_ls_build_list_cb_args *)cookie;
    copyinfo *ci;

    if (S_ISDIR(mode)) {
        copyinfo **dirlist = args->dirlist;

        // Don't try recursing down "." or "..".
        if (IsDotOrDotDot(name)) return;

        ci = mkcopyinfo(args->rpath, args->lpath, name, 1);
        ci->next = *dirlist;
        *dirlist = ci;
    } else if (S_ISREG(mode) || S_ISLNK(mode)) {
        copyinfo **filelist = args->filelist;

        ci = mkcopyinfo(args->rpath, args->lpath, name, 0);
        ci->time = time;
        ci->mode = mode;
        ci->size = size;
        ci->next = *filelist;
        *filelist = ci;
    } else {
        fprintf(stderr, "skipping special file '%s'\n", name);
    }
}

static bool remote_build_list(SyncConnection& sc, copyinfo **filelist,
                              const char *rpath, const char *lpath) {
    copyinfo *dirlist = NULL;
    sync_ls_build_list_cb_args args;

    args.filelist = filelist;
    args.dirlist = &dirlist;
    args.rpath = rpath;
    args.lpath = lpath;

    // Put the files/dirs in rpath on the lists.
    if (!sync_ls(sc, rpath, sync_ls_build_list_cb, (void *)&args)) {
        return false;
    }

    // Recurse into each directory we found.
    while (dirlist != NULL) {
        copyinfo *next = dirlist->next;
        if (!remote_build_list(sc, filelist, dirlist->src, dirlist->dst)) {
            return false;
        }
        free(dirlist);
        dirlist = next;
    }

    return true;
}

static int set_time_and_mode(const char *lpath, time_t time, unsigned int mode)
{
    struct utimbuf times = { time, time };
    int r1 = utime(lpath, &times);

    /* use umask for permissions */
    mode_t mask=umask(0000);
    umask(mask);
    int r2 = chmod(lpath, mode & ~mask);

    return r1 ? : r2;
}

static bool copy_remote_dir_local(SyncConnection& sc, const char* rpath, const char* lpath,
                                  int copy_attrs) {
    // Make sure that both directory paths end in a slash.
    std::string rpath_clean(rpath);
    std::string lpath_clean(lpath);
    if (rpath_clean.empty() || lpath_clean.empty()) return false;
    if (rpath_clean.back() != '/') rpath_clean.push_back('/');
    if (lpath_clean.back() != '/') lpath_clean.push_back('/');

    // Recursively build the list of files to copy.
    fprintf(stderr, "pull: building file list...\n");
    copyinfo* filelist = nullptr;
    if (!remote_build_list(sc, &filelist, rpath_clean.c_str(), lpath_clean.c_str())) return false;

    int pulled = 0;
    int skipped = 0;
    copyinfo* ci = filelist;
    while (ci) {
        copyinfo* next = ci->next;
        if (ci->flag == 0) {
            fprintf(stderr, "pull: %s -> %s\n", ci->src, ci->dst);
            if (!sync_recv(sc, ci->src, ci->dst, false)) {
                return false;
            }

            if (copy_attrs && set_time_and_mode(ci->dst, ci->time, ci->mode)) {
                return false;
            }
            pulled++;
        } else {
            skipped++;
        }
        free(ci);
        ci = next;
    }

    fprintf(stderr, "%d file%s pulled. %d file%s skipped.\n",
            pulled, (pulled == 1) ? "" : "s",
            skipped, (skipped == 1) ? "" : "s");
    return true;
}

bool do_sync_pull(const char* rpath, const char* lpath, bool show_progress, int copy_attrs) {
    SyncConnection sc;
    if (!sc.IsValid()) return false;

    unsigned mode, time;
    if (!sync_stat(sc, rpath, &time, &mode, nullptr)) return false;
    if (mode == 0) {
        fprintf(stderr, "remote object '%s' does not exist\n", rpath);
        return false;
    }

    if (S_ISREG(mode) || S_ISLNK(mode) || S_ISCHR(mode) || S_ISBLK(mode)) {
        std::string path_holder;
        struct stat st;
        if (stat(lpath, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                // If we're copying a remote file to a local directory,
                // we really want to copy to local_dir + "/" + basename(remote).
                path_holder = android::base::StringPrintf("%s/%s", lpath, adb_basename(rpath).c_str());
                lpath = path_holder.c_str();
            }
        }
        if (!sync_recv(sc, rpath, lpath, show_progress)) {
            return false;
        } else {
            if (copy_attrs && set_time_and_mode(lpath, time, mode)) {
                return false;
            }
        }
        return true;
    } else if (S_ISDIR(mode)) {
        return copy_remote_dir_local(sc, rpath, lpath, copy_attrs);
    }

    fprintf(stderr, "remote object '%s' not a file or directory\n", rpath);
    return false;
}

bool do_sync_sync(const std::string& lpath, const std::string& rpath, bool list_only) {
    fprintf(stderr, "syncing %s...\n", rpath.c_str());

    SyncConnection sc;
    if (!sc.IsValid()) return false;

    return copy_local_dir_remote(sc, lpath.c_str(), rpath.c_str(), true, list_only);
}
