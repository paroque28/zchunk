#include <argp.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <zck.h>

#include "util_common.h"

static char doc[] =
    "zckdlfs - Reconstruct a zchunk file from a local chunk directory";

static char args_doc[] = "<zchunk file>";

struct arguments {
    char *args[1];
    zck_log_type log_level;
    char *source;
    char *chunk_dir;
    bool exit;
};

static struct argp_option options[] = {
    {"verbose", 'v', 0, 0, "Increase verbosity"},
    {"quiet", 'q', 0, 0,
     "Only show warnings (can be specified twice to only show errors)"},
    {"source", 's', "FILE", 0, "File to use as delta source"},
    {"chunk-dir", 'd', "DIR", 0,
     "Directory containing compressed chunk files"},
    {"version", 'V', 0, 0, "Show program version"},
    {0}
};

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
    struct arguments *arguments = state->input;

    if(arguments->exit)
        return 0;

    switch(key) {
        case 'v':
            if(arguments->log_level > ZCK_LOG_INFO)
                arguments->log_level = ZCK_LOG_INFO;
            arguments->log_level--;
            if(arguments->log_level < ZCK_LOG_DDEBUG)
                arguments->log_level = ZCK_LOG_DDEBUG;
            break;
        case 'q':
            if(arguments->log_level < ZCK_LOG_INFO)
                arguments->log_level = ZCK_LOG_INFO;
            arguments->log_level += 1;
            if(arguments->log_level > ZCK_LOG_NONE)
                arguments->log_level = ZCK_LOG_NONE;
            break;
        case 's':
            arguments->source = arg;
            break;
        case 'd':
            arguments->chunk_dir = arg;
            break;
        case 'V':
            version();
            arguments->exit = true;
            break;
        case ARGP_KEY_ARG:
            if(state->arg_num >= 1) {
                argp_usage(state);
                return EINVAL;
            }
            arguments->args[state->arg_num] = arg;
            break;
        case ARGP_KEY_END:
            if(state->arg_num < 1) {
                argp_usage(state);
                return EINVAL;
            }
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = {options, parse_opt, args_doc, doc};

static char *chunk_path(const char *dir, const char *hash_name,
                        const char *digest) {
    size_t digest_len = strlen(digest);
    char prefix[3] = {0};
    if(digest_len >= 2) {
        prefix[0] = digest[0];
        prefix[1] = digest[1];
    } else {
        prefix[0] = 's';
        prefix[1] = 'm';
    }
    prefix[2] = '\0';

    size_t path_len = strlen(dir) + strlen(hash_name) + strlen(prefix) +
                      strlen(digest) + strlen("///.chunk") + 1;
    char *path = calloc(path_len, 1);
    if(path == NULL)
        return NULL;
    snprintf(path, path_len, "%s/%s/%s/%s.chunk", dir, hash_name, prefix,
             digest);
    return path;
}

int main(int argc, char *argv[]) {
    struct arguments arguments = {0};
    arguments.log_level = ZCK_LOG_INFO;

    int retval = argp_parse(&argp, argc, argv, 0, 0, &arguments);
    if(retval || arguments.exit)
        exit(retval);

    if(arguments.chunk_dir == NULL) {
        LOG_ERROR("--chunk-dir must be specified\n");
        exit(2);
    }

    zck_set_log_level(arguments.log_level);

    zckCtx *zck_src = NULL;
    int src_fd = -1;
    if(arguments.source) {
        src_fd = open(arguments.source, O_RDONLY | O_BINARY);
        if(src_fd < 0) {
            LOG_ERROR("Unable to open %s\n", arguments.source);
            perror(NULL);
            exit(10);
        }
        zck_src = zck_create();
        if(zck_src == NULL)
            exit(10);
        if(!zck_init_read(zck_src, src_fd)) {
            LOG_ERROR("Unable to open %s: %s\n", arguments.source,
                      zck_get_error(zck_src));
            exit(10);
        }
    }

    int dst_fd = open(arguments.args[0], O_RDWR | O_BINARY);
    if(dst_fd < 0) {
        LOG_ERROR("Unable to open %s: %s\n", arguments.args[0],
                  strerror(errno));
        exit(10);
    }

    zckCtx *zck_tgt = zck_create();
    if(zck_tgt == NULL)
        exit(10);
    if(!zck_init_adv_read(zck_tgt, dst_fd)) {
        LOG_ERROR("%s", zck_get_error(zck_tgt));
        exit(10);
    }
    if(!zck_read_lead(zck_tgt) || !zck_read_header(zck_tgt)) {
        LOG_ERROR("%s", zck_get_error(zck_tgt));
        exit(10);
    }

    if(zck_src && !zck_copy_chunks(zck_src, zck_tgt)) {
        LOG_ERROR("%s", zck_get_error(zck_tgt));
        exit(10);
    }

    int chk_ret = zck_find_valid_chunks(zck_tgt);
    if(chk_ret == 0) {
        LOG_ERROR("%s", zck_get_error(zck_tgt));
        exit(10);
    }
    if(chk_ret == 1) {
        printf("Missing chunks: 0\n");
        zck_free(&zck_tgt);
        zck_free(&zck_src);
        close(dst_fd);
        return 0;
    }

    zckDL *dl = zck_dl_init(zck_tgt);
    if(dl == NULL)
        exit(10);

    const char *hash_name =
        zck_hash_name_from_type(zck_get_chunk_hash_type(zck_tgt));
    if(hash_name == NULL) {
        LOG_ERROR("Unable to determine chunk hash type\n");
        exit(10);
    }

    ssize_t missing = zck_missing_chunks(zck_tgt);
    if(missing < 0) {
        LOG_ERROR("%s", zck_get_error(zck_tgt));
        exit(10);
    }
    printf("Missing chunks: %lli\n", (long long)missing);

    size_t copied_bytes = 0;
    bool error = false;

    while(true) {
        missing = zck_missing_chunks(zck_tgt);
        if(missing < 0) {
            LOG_ERROR("%s", zck_get_error(zck_tgt));
            error = true;
            break;
        }
        if(missing == 0)
            break;

        zckChunk *tgt_chunk = NULL;
        for(zckChunk *chk = zck_get_first_chunk(zck_tgt); chk;
            chk = zck_get_next_chunk(chk)) {
            int valid = zck_get_chunk_valid(chk);
            if(valid < 0) {
                tgt_chunk = chk;
                break;
            }
            if(valid == 0) {
                tgt_chunk = chk;
                break;
            }
        }
        if(tgt_chunk == NULL) {
            LOG_ERROR("Unable to locate missing chunk\n");
            error = true;
            break;
        }

        ssize_t comp_size = zck_get_chunk_comp_size(tgt_chunk);
        if(comp_size < 0) {
            LOG_ERROR("%s", zck_get_error(zck_tgt));
            error = true;
            break;
        }

        char *digest = zck_get_chunk_digest(tgt_chunk);
        if(digest == NULL) {
            LOG_ERROR("Unable to get chunk digest\n");
            error = true;
            break;
        }

        char *path = chunk_path(arguments.chunk_dir, hash_name, digest);
        if(path == NULL) {
            free(digest);
            error = true;
            break;
        }

        int chunk_fd = open(path, O_RDONLY | O_BINARY);
        if(chunk_fd < 0) {
            LOG_ERROR("Missing chunk %s in %s\n", digest, path);
            free(path);
            free(digest);
            error = true;
            break;
        }

        zck_dl_reset(dl);
        zckRange *range = zck_get_missing_range(zck_tgt, 1);
        if(range == NULL) {
            LOG_ERROR("%s", zck_get_error(zck_tgt));
            close(chunk_fd);
            free(path);
            free(digest);
            error = true;
            break;
        }
        if(!zck_dl_set_range(dl, range)) {
            LOG_ERROR("%s", zck_get_error(zck_tgt));
            zck_range_free(&range);
            close(chunk_fd);
            free(path);
            free(digest);
            error = true;
            break;
        }

        ssize_t remaining = comp_size;
        char buffer[BUF_SIZE];
        bool chunk_ok = true;
        while(remaining > 0) {
            ssize_t to_read = remaining > (ssize_t)sizeof(buffer)
                                  ? (ssize_t)sizeof(buffer)
                                  : remaining;
            ssize_t rb = read(chunk_fd, buffer, (size_t)to_read);
            if(rb <= 0) {
                LOG_ERROR("Error reading %s: %s\n", path, strerror(errno));
                chunk_ok = false;
                break;
            }
            size_t wb = zck_write_chunk_cb(buffer, 1, (size_t)rb, dl);
            if(wb != (size_t)rb) {
                LOG_ERROR("Failed to import chunk %s\n", digest);
                chunk_ok = false;
                break;
            }
            remaining -= rb;
        }

        if(!zck_dl_set_range(dl, NULL)) {
            LOG_ERROR("%s", zck_get_error(zck_tgt));
            zck_range_free(&range);
            close(chunk_fd);
            free(path);
            free(digest);
            error = true;
            break;
        }
        zck_range_free(&range);
        close(chunk_fd);

        if(!chunk_ok || remaining > 0) {
            free(path);
            free(digest);
            error = true;
            break;
        }

        copied_bytes += (size_t)comp_size;
        free(path);
        free(digest);
    }

    if(!error) {
        if(ftruncate(dst_fd, zck_get_length(zck_tgt)) < 0) {
            perror(NULL);
            error = true;
        }
    }

    if(!error) {
        int valid = zck_validate_data_checksum(zck_tgt);
        if(valid < 1) {
            if(valid == -1)
                LOG_ERROR("Data checksum failed verification\n");
            else
                LOG_ERROR("%s", zck_get_error(zck_tgt));
            error = true;
        }
    }

    if(!error) {
        printf("Copied %llu bytes from %s\n",
               (long long unsigned)copied_bytes, arguments.chunk_dir);
    }

    zck_dl_free(&dl);
    zck_free(&zck_tgt);
    zck_free(&zck_src);
    if(src_fd >= 0)
        close(src_fd);
    close(dst_fd);

    return error ? 1 : 0;
}
