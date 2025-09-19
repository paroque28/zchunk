#include <argp.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
#endif

#include <zck.h>

#include "util_common.h"

static char doc[] =
    "zck_chunk_store - Export compressed zchunk chunks to a chunk directory";

static char args_doc[] = "<zchunk file>";

struct arguments {
    char *args[1];
    zck_log_type log_level;
    char *output_dir;
    bool exit;
};

static struct argp_option options[] = {
    {"verbose", 'v', 0, 0, "Increase verbosity"},
    {"quiet", 'q', 0, 0,
     "Only show warnings (can be specified twice to only show errors)"},
    {"output", 'o', "DIR", 0, "Directory to populate with chunk files"},
    {"version", 'V', 0, 0, "Show program version"},
    {0}
};

static bool ensure_dir(const char *path) {
    struct stat st = {0};
    if(stat(path, &st) == 0) {
        if(S_ISDIR(st.st_mode))
            return true;
        errno = ENOTDIR;
        return false;
    }
#ifdef _WIN32
    if(_mkdir(path) == -1 && errno != EEXIST)
        return false;
#else
    if(mkdir(path, 0777) == -1 && errno != EEXIST)
        return false;
#endif
    return true;
}

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
        case 'o':
            arguments->output_dir = arg;
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

static char *build_chunk_path(const char *base, const char *hash_name,
                              const char *digest) {
    size_t digest_len = strlen(digest);
    const char *subdir = "zz";
    char prefix[3] = {0};
    if(digest_len >= 2) {
        prefix[0] = digest[0];
        prefix[1] = digest[1];
    } else {
        prefix[0] = 's';
        prefix[1] = 'm';
    }
    prefix[2] = '\0';
    subdir = prefix;

    size_t path_len = strlen(base) + strlen(hash_name) + strlen(subdir) +
                      strlen(digest) + strlen("///.chunk") + 1;
    char *path = calloc(path_len, 1);
    if(path == NULL)
        return NULL;
    snprintf(path, path_len, "%s/%s/%s/%s.chunk", base, hash_name, subdir,
             digest);
    return path;
}

int main(int argc, char *argv[]) {
    struct arguments arguments = {0};
    arguments.log_level = ZCK_LOG_INFO;

    int retval = argp_parse(&argp, argc, argv, 0, 0, &arguments);
    if(retval || arguments.exit)
        exit(retval);

    if(arguments.output_dir == NULL) {
        LOG_ERROR("--output must be specified\n");
        exit(2);
    }

    zck_set_log_level(arguments.log_level);

    int src_fd = open(arguments.args[0], O_RDONLY | O_BINARY);
    if(src_fd < 0) {
        LOG_ERROR("Unable to open %s\n", arguments.args[0]);
        perror(NULL);
        exit(10);
    }

    if(!ensure_dir(arguments.output_dir)) {
        LOG_ERROR("Unable to create %s: %s\n", arguments.output_dir,
                  strerror(errno));
        exit(10);
    }

    zckCtx *zck = zck_create();
    if(zck == NULL)
        exit(10);
    if(!zck_init_read(zck, src_fd)) {
        LOG_ERROR("%s", zck_get_error(zck));
        exit(10);
    }
    if(!zck_read_lead(zck) || !zck_read_header(zck)) {
        LOG_ERROR("%s", zck_get_error(zck));
        exit(10);
    }

    const char *hash_name = zck_hash_name_from_type(zck_get_chunk_hash_type(zck));
    if(hash_name == NULL) {
        LOG_ERROR("Unable to determine chunk hash type\n");
        exit(10);
    }

    char *hash_dir = NULL;
    size_t hash_dir_len = strlen(arguments.output_dir) + strlen(hash_name) + 2;
    hash_dir = calloc(hash_dir_len, 1);
    if(hash_dir == NULL)
        exit(10);
    snprintf(hash_dir, hash_dir_len, "%s/%s", arguments.output_dir, hash_name);
    if(!ensure_dir(hash_dir)) {
        LOG_ERROR("Unable to create %s: %s\n", hash_dir, strerror(errno));
        free(hash_dir);
        exit(10);
    }

    size_t chunk_count = 0;
    size_t bytes_written = 0;

    for(zckChunk *chunk = zck_get_first_chunk(zck); chunk;
        chunk = zck_get_next_chunk(chunk)) {
        char *digest = zck_get_chunk_digest(chunk);
        if(digest == NULL) {
            LOG_ERROR("Unable to get chunk digest\n");
            free(hash_dir);
            exit(10);
        }
        char prefix[3] = {0};
        size_t digest_len = strlen(digest);
        if(digest_len >= 2) {
            prefix[0] = digest[0];
            prefix[1] = digest[1];
        } else {
            prefix[0] = 's';
            prefix[1] = 'm';
        }
        prefix[2] = '\0';
        size_t prefix_dir_len = strlen(hash_dir) + strlen(prefix) + 2;
        char *prefix_dir = calloc(prefix_dir_len, 1);
        if(prefix_dir == NULL) {
            free(digest);
            free(hash_dir);
            exit(10);
        }
        snprintf(prefix_dir, prefix_dir_len, "%s/%s", hash_dir, prefix);
        if(!ensure_dir(prefix_dir)) {
            LOG_ERROR("Unable to create %s: %s\n", prefix_dir, strerror(errno));
            free(digest);
            free(prefix_dir);
            free(hash_dir);
            exit(10);
        }

        char *chunk_path = build_chunk_path(arguments.output_dir, hash_name,
                                            digest);
        if(chunk_path == NULL) {
            free(prefix_dir);
            free(digest);
            free(hash_dir);
            exit(10);
        }

        int chunk_fd =
            open(chunk_path, O_TRUNC | O_WRONLY | O_CREAT | O_BINARY, 0666);
        if(chunk_fd < 0) {
            LOG_ERROR("Unable to open %s: %s\n", chunk_path, strerror(errno));
            free(chunk_path);
            free(prefix_dir);
            free(digest);
            free(hash_dir);
            exit(10);
        }

        ssize_t comp_size = zck_get_chunk_comp_size(chunk);
        if(comp_size < 0) {
            LOG_ERROR("%s", zck_get_error(zck));
            close(chunk_fd);
            free(chunk_path);
            free(digest);
            free(hash_dir);
            exit(10);
        }

            if(comp_size > 0) {
                char *buffer = calloc(comp_size, 1);
                if(buffer == NULL) {
                    close(chunk_fd);
                    free(chunk_path);
                    free(prefix_dir);
                    free(digest);
                    free(hash_dir);
                    exit(10);
                }
                ssize_t read_size =
                zck_get_chunk_comp_data(chunk, buffer, (size_t)comp_size);
                if(read_size != comp_size) {
                    if(read_size < 0)
                        LOG_ERROR("%s", zck_get_error(zck));
                    else
                        LOG_ERROR(
                        "Chunk %lli size mismatch: expected %lli, read %lli\n",
                        (long long)zck_get_chunk_number(chunk),
                        (long long)comp_size,
                        (long long)read_size);
                free(buffer);
                close(chunk_fd);
                free(chunk_path);
                free(prefix_dir);
                free(digest);
                free(hash_dir);
                exit(10);
            }
            if(write(chunk_fd, buffer, comp_size) != comp_size) {
                LOG_ERROR("Unable to write %s: %s\n", chunk_path,
                          strerror(errno));
                free(buffer);
                close(chunk_fd);
                free(chunk_path);
                free(prefix_dir);
                free(digest);
                free(hash_dir);
                exit(10);
            }
            bytes_written += comp_size;
            free(buffer);
        }

        close(chunk_fd);
        free(chunk_path);
        free(prefix_dir);
        free(digest);
        chunk_count++;
    }

    printf("Stored %llu chunks (%llu bytes) in %s\n",
           (long long unsigned)chunk_count,
           (long long unsigned)bytes_written,
           arguments.output_dir);

    free(hash_dir);
    zck_free(&zck);
    close(src_fd);
    return 0;
}
