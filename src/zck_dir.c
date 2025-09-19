/*
 * Offline chunk applier for zchunk files.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <argp.h>
#include <zck.h>

#include "util_common.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

static char doc[] = "zckdir - Rebuild a zchunk file using cached chunks";

static char args_doc[] = "<output-file>";

static struct argp_option options[] = {
    {"verbose",    'v',  0, 0, "Increase verbosity"},
    {"quiet",      'q',  0, 0,
     "Only show warnings (can be specified twice to only show errors)"},
    {"source",     's', "FILE", 0, "File to use as delta source"},
    {"chunk-dir",  'C', "DIR", 0, "Directory containing cached chunks"},
    {"header",     'H', "FILE", 0, "Header file to seed the output"},
    {"version",    'V',  0, 0, "Show program version"},
    { 0 }
};

struct missing_entry {
    char *digest;
    bool uncompressed;
};

struct arguments {
    char *output;
    char *chunk_dir;
    char *source;
    char *header;
    struct missing_entry *missing;
    size_t missing_count;
    size_t missing_alloc;
    zck_log_type log_level;
    bool exit;
};

static bool write_all(int fd, const char *buf, size_t length) {
    while(length > 0) {
        ssize_t written = write(fd, buf, length);
        if(written < 0)
            return false;
        buf += written;
        length -= written;
    }
    return true;
}

static bool copy_file(const char *src, const char *dst) {
    int in_fd = open(src, O_RDONLY | O_BINARY);
    if(in_fd < 0) {
        perror(src);
        return false;
    }
    int out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
    if(out_fd < 0) {
        perror(dst);
        close(in_fd);
        return false;
    }

    char buf[BUF_SIZE];
    bool ok = true;
    for(;;) {
        ssize_t rb = read(in_fd, buf, sizeof(buf));
        if(rb < 0) {
            perror(src);
            ok = false;
            break;
        }
        if(rb == 0)
            break;
        if(!write_all(out_fd, buf, rb)) {
            perror(dst);
            ok = false;
            break;
        }
    }
    close(in_fd);
    close(out_fd);
    return ok;
}

static bool digest_all_zero(const char *digest) {
    if(!digest)
        return false;
    for(const char *c = digest; *c; c++)
        if(*c != '0')
            return false;
    return true;
}

static void hash_to_dir_name(const char *hash_name, char *dst, size_t dst_len) {
    size_t j = 0;
    for(size_t i = 0; hash_name[i] && j + 1 < dst_len; i++) {
        unsigned char c = (unsigned char)hash_name[i];
        if(isalnum(c))
            dst[j++] = tolower(c);
        else
            dst[j++] = '_';
    }
    dst[j] = '\0';
}

static bool ensure_meta(const char *chunk_dir, zckCtx *zck) {
    char meta_path[PATH_MAX];
    snprintf(meta_path, sizeof(meta_path), "%s/meta.conf", chunk_dir);
    FILE *fp = fopen(meta_path, "r");
    if(!fp) {
        fprintf(stderr, "Unable to open %s\n", meta_path);
        return false;
    }
    char expected_hash[128] = {0};
    ssize_t expected_size = -1;
    char line[256];
    while(fgets(line, sizeof(line), fp)) {
        if(strncmp(line, "chunk_hash=", 11) == 0) {
            strncpy(expected_hash, line + 11, sizeof(expected_hash)-1);
            char *nl = strchr(expected_hash, '\n');
            if(nl)
                *nl = '\0';
        } else if(strncmp(line, "chunk_digest_size=", 18) == 0) {
            expected_size = strtol(line + 18, NULL, 10);
        }
    }
    fclose(fp);

    const char *hash_name = zck_hash_name_from_type(zck_get_chunk_hash_type(zck));
    ssize_t digest_size = zck_get_chunk_digest_size(zck);
    if(expected_hash[0] == '\0' || expected_size < 0) {
        fprintf(stderr, "Chunk store metadata in %s is incomplete (hash='%s', size=%lli)\n",
                meta_path, expected_hash, (long long)expected_size);
        return false;
    }
    if(strcmp(expected_hash, hash_name) != 0 || expected_size != digest_size) {
        fprintf(stderr, "Chunk store metadata (%s/%lli) doesn't match target (%s/%lli)\n",
                expected_hash, (long long)expected_size,
                hash_name, (long long)digest_size);
        return false;
    }
    return true;
}

static bool build_chunk_path(char *dst, size_t dst_len, const char *chunk_dir,
                             const char *hash_name, const char *digest,
                             bool uncompressed) {
    if(strlen(digest) < 2)
        return false;
    int needed = snprintf(dst, dst_len, "%s/%s/%s/%c%c/%s.chunk",
                          chunk_dir,
                          uncompressed ? "chunks-uncompressed" : "chunks",
                          hash_name, digest[0], digest[1], digest);
    return needed > 0 && (size_t)needed < dst_len;
}

static bool add_missing(struct arguments *arguments, const char *digest,
                        bool uncompressed) {
    if(arguments->missing_count + 1 >= arguments->missing_alloc) {
        size_t new_alloc = arguments->missing_alloc ?
                           arguments->missing_alloc * 2 : 8;
        struct missing_entry *tmp = realloc(arguments->missing,
                                            new_alloc * sizeof(*tmp));
        if(!tmp)
            return false;
        arguments->missing = tmp;
        arguments->missing_alloc = new_alloc;
    }
    arguments->missing[arguments->missing_count].digest = strdup(digest);
    if(!arguments->missing[arguments->missing_count].digest)
        return false;
    arguments->missing[arguments->missing_count].uncompressed = uncompressed;
    arguments->missing_count++;
    return true;
}

static error_t parse_opt (int key, char *arg, struct argp_state *state) {
    struct arguments *arguments = state->input;

    if(arguments->exit)
        return 0;

    switch (key) {
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
        case 'C':
            arguments->chunk_dir = arg;
            break;
        case 'H':
            arguments->header = arg;
            break;
        case 'V':
            version();
            arguments->exit = true;
            break;
        case ARGP_KEY_ARG:
            if(state->arg_num > 0) {
                argp_usage(state);
                return EINVAL;
            }
            arguments->output = arg;
            break;
        case ARGP_KEY_END:
            if(!arguments->chunk_dir || !arguments->output) {
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

int main(int argc, char **argv) {
    struct arguments arguments = {0};
    arguments.log_level = ZCK_LOG_ERROR;

    int retval = argp_parse(&argp, argc, argv, 0, 0, &arguments);
    if(retval || arguments.exit)
        exit(retval);

    zck_set_log_level(arguments.log_level);

    if(arguments.header) {
        if(!copy_file(arguments.header, arguments.output))
            exit(1);
    }

    int dst_fd = open(arguments.output, O_RDWR | O_BINARY);
    if(dst_fd < 0) {
        perror(arguments.output);
        exit(1);
    }

    zckCtx *zck_tgt = zck_create();
    if(!zck_tgt) {
        close(dst_fd);
        exit(1);
    }
    if(!zck_init_adv_read(zck_tgt, dst_fd)) {
        LOG_ERROR("%s", zck_get_error(zck_tgt));
        zck_free(&zck_tgt);
        close(dst_fd);
        exit(1);
    }
    if(!zck_read_lead(zck_tgt) || !zck_read_header(zck_tgt)) {
        LOG_ERROR("%s", zck_get_error(zck_tgt));
        zck_free(&zck_tgt);
        close(dst_fd);
        exit(1);
    }

    if(!ensure_meta(arguments.chunk_dir, zck_tgt)) {
        zck_free(&zck_tgt);
        close(dst_fd);
        exit(1);
    }

    zckCtx *zck_src = NULL;
    int src_fd = -1;
    if(arguments.source) {
        src_fd = open(arguments.source, O_RDONLY | O_BINARY);
        if(src_fd < 0) {
            perror(arguments.source);
            zck_free(&zck_tgt);
            close(dst_fd);
            exit(1);
        }
        zck_src = zck_create();
        if(!zck_src) {
            close(src_fd);
            zck_free(&zck_tgt);
            close(dst_fd);
            exit(1);
        }
        if(!zck_init_read(zck_src, src_fd)) {
            LOG_ERROR("Unable to open %s: %s", arguments.source,
                      zck_get_error(zck_src));
            zck_free(&zck_src);
            close(src_fd);
            zck_free(&zck_tgt);
            close(dst_fd);
            exit(1);
        }
    }

    int valid = zck_find_valid_chunks(zck_tgt);
    if(valid == 0) {
        LOG_ERROR("%s", zck_get_error(zck_tgt));
        zck_free(&zck_src);
        if(src_fd >= 0)
            close(src_fd);
        zck_free(&zck_tgt);
        close(dst_fd);
        exit(1);
    }

    size_t imported = 0;
    int remaining = 0;

    if(valid != 1) {
        if(zck_src && !zck_copy_chunks(zck_src, zck_tgt)) {
            LOG_ERROR("%s", zck_get_error(zck_tgt));
            zck_free(&zck_src);
            if(src_fd >= 0)
                close(src_fd);
            zck_free(&zck_tgt);
            close(dst_fd);
            exit(1);
        }

        zck_reset_failed_chunks(zck_tgt);

    const char *hash_name = zck_hash_name_from_type(zck_get_chunk_hash_type(zck_tgt));
    char hash_dir[128];
    hash_to_dir_name(hash_name, hash_dir, sizeof(hash_dir));
        ssize_t chunk_count = zck_get_chunk_count(zck_tgt);
        for(ssize_t i = 0; i < chunk_count; i++) {
            zckChunk *chunk = zck_get_chunk(zck_tgt, i);
            if(!chunk)
                continue;
            if(zck_get_chunk_valid(chunk) == 1)
                continue;
            ssize_t comp_len = zck_get_chunk_comp_size(chunk);
            if(comp_len == 0)
                continue;
            char *digest = zck_get_chunk_digest(chunk);
            bool uncompressed = false;
            if(!digest) {
                LOG_ERROR("%s", zck_get_error(zck_tgt));
                continue;
            }
            if(digest_all_zero(digest)) {
                free(digest);
                digest = zck_get_chunk_digest_uncompressed(chunk);
                uncompressed = true;
                if(!digest) {
                    LOG_ERROR("%s", zck_get_error(zck_tgt));
                    continue;
                }
            }

            char chunk_path[PATH_MAX];
            if(!build_chunk_path(chunk_path, sizeof(chunk_path), arguments.chunk_dir,
                                  hash_dir, digest, uncompressed)) {
                if(!add_missing(&arguments, digest, uncompressed)) {
                    fprintf(stderr, "Unable to record missing chunk\n");
                    free(digest);
                    zck_free(&zck_src);
                    if(src_fd >= 0)
                        close(src_fd);
                    zck_free(&zck_tgt);
                    close(dst_fd);
                    exit(1);
                }
                free(digest);
                continue;
            }
            int cfd = open(chunk_path, O_RDONLY | O_BINARY);
            if(cfd < 0) {
                if(!add_missing(&arguments, digest, uncompressed)) {
                    fprintf(stderr, "Unable to record missing chunk\n");
                    free(digest);
                    zck_free(&zck_src);
                    if(src_fd >= 0)
                        close(src_fd);
                    zck_free(&zck_tgt);
                    close(dst_fd);
                    exit(1);
                }
                free(digest);
                continue;
            }
            if(!zck_import_chunk_from_fd(zck_tgt, chunk, cfd)) {
                if(!add_missing(&arguments, digest, uncompressed)) {
                    fprintf(stderr, "Unable to record missing chunk\n");
                    close(cfd);
                    free(digest);
                    zck_free(&zck_src);
                    if(src_fd >= 0)
                        close(src_fd);
                    zck_free(&zck_tgt);
                    close(dst_fd);
                    exit(1);
                }
            } else {
                imported += (size_t)comp_len;
            }
            close(cfd);
            free(digest);
        }

        remaining = zck_missing_chunks(zck_tgt);
    } else {
        remaining = 0;
    }

    printf("Missing chunks: %i\n", remaining);
    printf("Imported %llu bytes from chunk directory\n",
           (long long unsigned)imported);

    int exit_code = 0;
    if(remaining > 0) {
        exit_code = 1;
        if(arguments.missing_count > 0) {
            printf("Chunks still required (%zu):\n", arguments.missing_count);
            for(size_t i = 0; i < arguments.missing_count; i++) {
                printf("    %s%s\n",
                       arguments.missing[i].digest,
                       arguments.missing[i].uncompressed ? " (uncompressed)" : "");
            }
        }
    } else {
        if(ftruncate(dst_fd, zck_get_length(zck_tgt)) < 0) {
            perror("ftruncate");
            zck_free(&zck_src);
            if(src_fd >= 0)
                close(src_fd);
            zck_free(&zck_tgt);
            close(dst_fd);
            exit(1);
        }
        switch(zck_validate_data_checksum(zck_tgt)) {
            case -1:
            case 0:
                LOG_ERROR("%s", zck_get_error(zck_tgt));
                zck_free(&zck_src);
                if(src_fd >= 0)
                    close(src_fd);
                zck_free(&zck_tgt);
                close(dst_fd);
                exit(1);
            default:
                break;
        }
        printf("Data checksum OK\n");
    }

    zck_free(&zck_src);
    if(src_fd >= 0)
        close(src_fd);
    zck_free(&zck_tgt);
    close(dst_fd);
    for(size_t i = 0; i < arguments.missing_count; i++)
        free(arguments.missing[i].digest);
    free(arguments.missing);
    return exit_code;
}
