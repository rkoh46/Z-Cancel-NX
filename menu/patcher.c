// SPDX-License-Identifier: GPL-3.0-or-later
/* See patcher.h. */

#if defined(__GNUC__) && !defined(__clang__)
/* File names and paths are cut to the size of their field on purpose. */
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#include "patcher.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <curl/curl.h>

#include "catalog.h"
#include "md5.h"
#include "unzip.h"
#include "xdelta_apply.h"

#if !defined(ZNX_VERSION) && defined(__has_include)
#if __has_include("znx_version.h")
#include "znx_version.h"   /* the Switch build's generated version */
#endif
#endif
#ifndef ZNX_VERSION
#define ZNX_VERSION "dev"
#endif

static void fail(struct znx_job* job, const char* title, const char* format, ...) __attribute__((format(printf, 3, 4)));

#include <stdarg.h>
static void fail(struct znx_job* job, const char* title, const char* format, ...)
{
    va_list args;
    snprintf(job->error_title, sizeof(job->error_title), "%s", title);
    va_start(args, format);
    vsnprintf(job->error_text, sizeof(job->error_text), format, args);
    va_end(args);
    job->status = ZNX_JOB_FAILED;
}

static int cancelled(struct znx_job* job)
{
    if (job->cancel) {
        job->status = ZNX_JOB_CANCELLED;
        return 1;
    }
    return 0;
}

uint64_t znx_free_space(const char* path)
{
    struct statvfs info;
    if (statvfs(path, &info) != 0)
        return 0;
    return (uint64_t)info.f_bavail * (uint64_t)info.f_frsize;
}

static int progress_cb(void* user, uint64_t done, uint64_t total)
{
    struct znx_job* job = user;
    job->done = done;
    job->total = total;
    return job->cancel;
}

/* ---- step 1: download and unpack ---- */

struct download {
    struct znx_job* job;
    FILE* file;
    uint64_t written;
    int write_failed;
};

static size_t write_cb(char* data, size_t size, size_t count, void* user)
{
    struct download* d = user;
    size_t n = size * count;
    if (fwrite(data, 1, n, d->file) != n) {
        d->write_failed = 1;
        return 0;
    }
    d->written += n;
    return n;
}

static int transfer_cb(void* user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    struct download* d = user;
    (void)ultotal; (void)ulnow;
    d->job->done = (uint64_t)dlnow;
    if (dltotal > 0)
        d->job->total = (uint64_t)dltotal;
    return d->job->cancel ? 1 : 0;
}

/* Plain words for what curl reports. */
static void download_failed(struct znx_job* job, CURLcode code, long http, const char* detail)
{
    const char* title = "The patch could not be downloaded";
    switch (code) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
        fail(job, title, "GitHub could not be reached. Check that the console is connected to the internet, "
             "or copy the patch into the patches folder yourself (%s).", detail);
        break;
    case CURLE_OPERATION_TIMEDOUT:
        fail(job, title, "The connection stopped answering. Try again, or copy the patch into the patches folder yourself.");
        break;
    case CURLE_HTTP_RETURNED_ERROR:
        fail(job, title, "GitHub answered with error %ld: the file is not at the address this version knows. "
             "Copy the patch into the patches folder yourself.", http);
        break;
    case CURLE_WRITE_ERROR:
        fail(job, title, "It could not be written to the SD card. Check that the card has room and is not locked.");
        break;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
        fail(job, title, "The secure connection to GitHub failed (%s). Check the console's date and time.", detail);
        break;
    default:
        fail(job, title, "The download stopped: %s.", detail);
        break;
    }
}

static int download(struct znx_job* job, const struct znx_game_info* game, const char* zip_path)
{
    struct download d;
    CURL* curl;
    CURLcode code;
    long http = 0;
    char detail[CURL_ERROR_SIZE] = "";
    const char* url = getenv("ZNX_PATCH_URL_OVERRIDE"); /* the Mac preview tests with a file:// address */

    if (url == NULL || *url == '\0')
        url = game->zip_url;
    memset(&d, 0, sizeof(d));
    d.job = job;
    d.file = fopen(zip_path, "wb");
    if (d.file == NULL) {
        fail(job, "The patch could not be downloaded", "Cannot write %s: %s.", zip_path, strerror(errno));
        return 0;
    }
    curl = curl_easy_init();
    if (curl == NULL) {
        fclose(d.file);
        remove(zip_path);
        fail(job, "The patch could not be downloaded", "The network library could not start.");
        return 0;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);   /* GitHub sends the file from another host */
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Z-Cancel-NX/" ZNX_VERSION);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 512L);   /* under 512 bytes/s for 30 s: give up */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &d);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &d);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, detail);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 128L * 1024);
    job->total = game->zip_size;
    code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    if (fclose(d.file) != 0 && code == CURLE_OK)
        code = CURLE_WRITE_ERROR;
    if (d.write_failed)
        code = CURLE_WRITE_ERROR;
    if (code == CURLE_ABORTED_BY_CALLBACK || job->cancel) {
        remove(zip_path);
        job->status = ZNX_JOB_CANCELLED;
        return 0;
    }
    if (code != CURLE_OK) {
        remove(zip_path);
        if (detail[0] == '\0')
            snprintf(detail, sizeof(detail), "%s", curl_easy_strerror(code));
        download_failed(job, code, http, detail);
        return 0;
    }
    return 1;
}

static const char* base_name(const char* path)
{
    const char* slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

/* The .xdelta out of the zip, by its name in any folder of the zip; only that file is written. */
static int unpack(struct znx_job* job, const struct znx_game_info* game, const char* zip_path, const char* out_path)
{
    unzFile zip = unzOpen(zip_path);
    unz_file_info info;
    char name[512];
    int found = 0, ok = 0;
    FILE* out;
    uint8_t* buffer;

    if (zip == NULL) {
        fail(job, "The download is not a zip file", "%s could not be opened as a zip. It was deleted; try again.", base_name(zip_path));
        return 0;
    }
    for (int r = unzGoToFirstFile(zip); r == UNZ_OK; r = unzGoToNextFile(zip)) {
        if (unzGetCurrentFileInfo(zip, &info, name, sizeof(name), NULL, 0, NULL, 0) != UNZ_OK)
            break;
        if (strcmp(base_name(name), game->patch_name) == 0) {
            found = 1;
            break;
        }
    }
    if (!found) {
        unzClose(zip);
        fail(job, "The patch is not in the download", "%s has no file called %s.", base_name(zip_path), game->patch_name);
        return 0;
    }
    if (unzOpenCurrentFile(zip) != UNZ_OK || (out = fopen(out_path, "wb")) == NULL) {
        unzClose(zip);
        fail(job, "The patch could not be unpacked", "Cannot write %s.", out_path);
        return 0;
    }
    buffer = malloc(1 << 20);
    job->done = 0;
    job->total = info.uncompressed_size;
    for (;;) {
        int got = buffer != NULL ? unzReadCurrentFile(zip, buffer, 1 << 20) : -1;
        if (got < 0) {
            fail(job, "The patch could not be unpacked", "The zip is damaged. It was deleted; try again.");
            break;
        }
        if (got == 0) {
            ok = 1;
            break;
        }
        if (fwrite(buffer, 1, (size_t)got, out) != (size_t)got) {
            fail(job, "The patch could not be unpacked", "Cannot write %s (is the SD card full?).", out_path);
            break;
        }
        job->done += (uint64_t)got;
        if (cancelled(job))
            break;
    }
    free(buffer);
    if (unzCloseCurrentFile(zip) != UNZ_OK && ok) {
        fail(job, "The patch could not be unpacked", "The zip's checksum for %s is wrong. It was deleted; try again.", game->patch_name);
        ok = 0;
    }
    unzClose(zip);
    if (fclose(out) != 0 && ok) {
        fail(job, "The patch could not be unpacked", "Cannot write %s (is the SD card full?).", out_path);
        ok = 0;
    }
    return ok;
}

static int fetch_patch(struct znx_job* job, const struct znx_game_info* game)
{
    char folder[600], zip_path[700], part_path[700], patch_path[700], md5[33];
    struct stat info;

    snprintf(folder, sizeof(folder), "%s/patches", job->base);
    mkdir(folder, 0777);
    snprintf(zip_path, sizeof(zip_path), "%s/%s.part", folder, game->zip_name);
    snprintf(part_path, sizeof(part_path), "%s/%s.part", folder, game->patch_name);
    snprintf(patch_path, sizeof(patch_path), "%s/%s", folder, game->patch_name);

    if (!download(job, game, zip_path))
        return 0;
    if (stat(zip_path, &info) != 0 || (uint64_t)info.st_size != game->zip_size) {
        remove(zip_path);
        fail(job, "The download is not the release file",
             "GitHub sent %llu bytes; %s is %llu bytes. Nothing was kept; try again.",
             (unsigned long long)(stat(zip_path, &info) == 0 ? info.st_size : 0), game->zip_name,
             (unsigned long long)game->zip_size);
        return 0;
    }
    job->done = 0;
    job->total = game->zip_size;
    if (znx_md5_file(zip_path, 0, md5, progress_cb, job) != 0 || strcmp(md5, game->zip_md5) != 0) {
        remove(zip_path);
        if (cancelled(job))
            return 0;
        fail(job, "The download is not the release file",
             "Its MD5 is %s, the release's is %s. It was deleted.", md5, game->zip_md5);
        return 0;
    }
    if (!unpack(job, game, zip_path, part_path)) {
        remove(zip_path);
        remove(part_path);
        return 0;
    }
    remove(zip_path);
    remove(patch_path);
    if (rename(part_path, patch_path) != 0) {
        remove(part_path);
        fail(job, "The patch could not be saved", "Cannot rename %s: %s.", part_path, strerror(errno));
        return 0;
    }
    snprintf(job->patch_path, sizeof(job->patch_path), "%s", patch_path);
    return 1;
}

/* ---- steps 2 and 3 ---- */

static void build(struct znx_job* job, const struct znx_game_info* game)
{
    const struct znx_game_info* vanilla = znx_game_info(0);
    char folder[600], part_path[700], out_path[700], md5[33], error[512];
    uint8_t* rom;
    size_t rom_size = 0;
    struct stat info;
    uint64_t written = 0, free_bytes;
    int result;

    job->step = ZNX_STEP_BUILD;
    job->done = 0;
    job->total = game->patch_size;
    snprintf(folder, sizeof(folder), "%s/roms", job->base);
    mkdir(folder, 0777);
    snprintf(part_path, sizeof(part_path), "%s/%s.part", folder, game->out_name);
    snprintf(out_path, sizeof(out_path), "%s/%s", folder, game->out_name);

    free_bytes = znx_free_space(folder);
    if (free_bytes != 0 && free_bytes < game->size + (4u << 20)) {
        char need[32], have[32];
        znx_format_size(game->size, need, sizeof(need));
        znx_format_size(free_bytes, have, sizeof(have));
        fail(job, "The SD card is too full", "The ROM needs %s and the card has %s free.", need, have);
        return;
    }

    /* The patch first: a wrong one is the likelier problem and costs only a read. */
    if (stat(job->patch_path, &info) != 0 || (uint64_t)info.st_size != game->patch_size) {
        fail(job, "The patch is not the right file", "%s should be %llu bytes.", base_name(job->patch_path),
             (unsigned long long)game->patch_size);
        return;
    }
    if (znx_md5_file(job->patch_path, 0, md5, progress_cb, job) != 0 || strcmp(md5, game->patch_md5) != 0) {
        if (cancelled(job))
            return;
        fail(job, "The patch is not the right file", "%s has MD5 %s; the release's patch has %s. Delete it and "
             "let the app download the patch again.", base_name(job->patch_path), md5, game->patch_md5);
        return;
    }

    rom = znx_rom_load(job->vanilla_path, &rom_size, NULL, error, sizeof(error));
    if (rom == NULL) {
        fail(job, "Your ROM could not be read", "%s", error);
        return;
    }
    znx_md5_buffer(rom, rom_size, md5);
    if (strcmp(md5, vanilla->md5) != 0) {
        free(rom);
        fail(job, "Your ROM has changed", "%s now has MD5 %s instead of %s.", base_name(job->vanilla_path), md5, vanilla->md5);
        return;
    }

    job->done = 0;
    job->total = game->patch_size;
    result = znx_xdelta_apply(rom, rom_size, job->patch_path, part_path, md5, &written, progress_cb, job, error, sizeof(error));
    free(rom);
    if (result == 2 || job->cancel) {
        remove(part_path);
        job->status = ZNX_JOB_CANCELLED;
        return;
    }
    if (result != 0) {
        remove(part_path);
        fail(job, "The ROM could not be built", "%s", error);
        return;
    }

    job->step = ZNX_STEP_CHECK;
    job->done = job->total = 1;
    snprintf(job->result_md5, sizeof(job->result_md5), "%s", md5);
    if (strcmp(md5, game->md5) != 0 || written != game->size) {
        remove(part_path);
        fail(job, "The built ROM is wrong and was deleted",
             "It came out as %llu bytes with MD5 %s; %s is %llu bytes with MD5 %s. Other players would not match it.",
             (unsigned long long)written, md5, game->title, (unsigned long long)game->size, game->md5);
        return;
    }
    remove(out_path);
    if (rename(part_path, out_path) != 0) {
        remove(part_path);
        fail(job, "The ROM could not be saved", "Cannot rename %s: %s.", part_path, strerror(errno));
        return;
    }
    znx_catalog_remember(job->base, out_path, md5);
    snprintf(job->result_path, sizeof(job->result_path), "%s", out_path);
    job->status = ZNX_JOB_DONE;
}

void znx_job_run(struct znx_job* job)
{
    const struct znx_game_info* game = znx_game_info(job->game);
    job->status = ZNX_JOB_RUNNING;
    job->error_title[0] = job->error_text[0] = '\0';
    job->result_path[0] = job->result_md5[0] = '\0';
    if (game == NULL || game->patch_name == NULL) {
        fail(job, "Nothing to build", "This game is played from your own ROM.");
        return;
    }
    job->downloads = job->patch_path[0] == '\0';
    if (job->downloads) {
        job->step = ZNX_STEP_DOWNLOAD;
        if (!fetch_patch(job, game))
            return;
    }
    if (cancelled(job))
        return;
    build(job, game);
}

static void* job_thread(void* arg)
{
    znx_job_run(arg);
    return NULL;
}

int znx_job_start(struct znx_job* job)
{
    static int curl_ready;
    pthread_attr_t attr;
    pthread_t* thread = malloc(sizeof(*thread));
    if (!curl_ready) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_ready = 1;
    }
    job->status = ZNX_JOB_RUNNING;
    job->step = job->patch_path[0] == '\0' ? ZNX_STEP_DOWNLOAD : ZNX_STEP_BUILD;
    job->downloads = job->patch_path[0] == '\0';
    job->done = job->total = 0;
    job->cancel = 0;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20);
    if (thread == NULL || pthread_create(thread, &attr, job_thread, job) != 0) {
        pthread_attr_destroy(&attr);
        free(thread);
        job->thread = NULL;
        fail(job, "The build could not start", "A thread could not be created.");
        return 0;
    }
    pthread_attr_destroy(&attr);
    job->thread = thread;
    return 1;
}

void znx_job_finish(struct znx_job* job)
{
    if (job->thread != NULL) {
        pthread_join(*(pthread_t*)job->thread, NULL);
        free(job->thread);
        job->thread = NULL;
    }
}
