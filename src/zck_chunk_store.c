/*
 * Offline chunk store generator for zchunk files.
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

static char doc[] = "zck_chunk_store - Export compressed chunks into a directory cache";

static char args_doc[] = "<chunk-dir> <file> [file...]";

static struct argp_option options[] = {
    {"verbose",        'v',  0, 0, "Increase verbosity"},
    {"quiet",          'q',  0, 0,
     "Only show warnings (can be specified twice to only show errors)"},
    {"force",          'f',  0, 0, "Overwrite existing chunk files"},
    {"version",        'V',  0, 0, "Show program version"},
    { 0 }
};

struct arguments {
    char *chunk_dir;
    char **inputs;
    size_t input_count;
    size_t input_alloc;
    zck_log_type log_level;
    bool force;
    bool exit;
};

static bool ensure_dir(const char *path) {
    struct stat st = {0};
    if(stat(path, &st) == 0) {
        if(S_ISDIR(st.st_mode))
            return true;
        fprintf(stderr, "%s exists and is not a directory\n", path);
        return false;
    }
    if(errno != ENOENT) {
        perror(path);
        return false;
    }
    if(mkdir(path, 0777) < 0) {
        if(errno == EEXIST)
            return true;
        perror(path);
        return false;
    }
    return true;
}

static const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if(bslash && (!slash || bslash > slash))
        slash = bslash;
#endif
    if(!slash)
        return path;
    return slash + 1;
}

static bool write_all(int fd, const char *buf, size_t length) {
    while(length > 0) {
        ssize_t written = write(fd, buf, length);
        if(written < 0) {
            return false;
        }
        buf += written;
        length -= written;
    }
    return true;
}

static bool read_exact(int fd, char *buf, size_t length) {
    while(length > 0) {
        ssize_t rb = read(fd, buf, length);
        if(rb < 0)
            return false;
        if(rb == 0)
            return false;
        buf += rb;
        length -= rb;
    }
    return true;
}

static bool read_meta(const char *path, char *hash_name, size_t hash_len,
                      ssize_t *digest_size) {
    FILE *fp = fopen(path, "r");
    if(!fp)
        return false;

    char line[256];
    while(fgets(line, sizeof(line), fp)) {
        if(strncmp(line, "chunk_hash=", 11) == 0) {
            strncpy(hash_name, line + 11, hash_len - 1);
            hash_name[hash_len-1] = '\0';
            char *nl = strchr(hash_name, '\n');
            if(nl)
                *nl = '\0';
        } else if(strncmp(line, "chunk_digest_size=", 19) == 0) {
            *digest_size = strtol(line + 19, NULL, 10);
        }
    }
    fclose(fp);
    return hash_name[0] != '\0' && *digest_size >= 0;
}

static bool write_meta(const char *dir, zckCtx *zck, bool force) {
    const char *hash_name = zck_hash_name_from_type(zck_get_chunk_hash_type(zck));
    ssize_t digest_size = zck_get_chunk_digest_size(zck);

    char meta_path[PATH_MAX];
    snprintf(meta_path, sizeof(meta_path), "%s/meta.conf", dir);

    char existing_hash[128] = {0};
    ssize_t existing_digest = -1;
    if(read_meta(meta_path, existing_hash, sizeof(existing_hash), &existing_digest)) {
        if(strcmp(existing_hash, hash_name) != 0 || existing_digest != digest_size) {
            fprintf(stderr,
                    "Existing metadata in %s is incompatible with %s\n",
                    meta_path, hash_name);
            return false;
        }
        if(!force)
            return true;
    }

    FILE *fp = fopen(meta_path, "w");
    if(!fp) {
        perror(meta_path);
        return false;
    }
    fprintf(fp, "chunk_hash=%s\n", hash_name);
    fprintf(fp, "chunk_digest_size=%lli\n", (long long)digest_size);
    fclose(fp);
    return true;
}

static bool export_header(zckCtx *zck, const char *chunk_dir, const char *input) {
    ssize_t header_len = zck_get_header_length(zck);
    if(header_len <= 0) {
        fprintf(stderr, "Unable to determine header length for %s\n", input);
        return false;
    }

    size_t header_size = (size_t)header_len;
    int fd = zck_get_fd(zck);
    if(lseek(fd, 0, SEEK_SET) < 0) {
        perror("lseek");
        return false;
    }

    char *buffer = malloc(header_size);
    if(!buffer) {
        fprintf(stderr, "Unable to allocate %lli bytes for header\n",
                (long long)header_len);
        return false;
    }

    if(!read_exact(fd, buffer, header_size)) {
        fprintf(stderr, "Short read when exporting header for %s\n", input);
        free(buffer);
        return false;
    }
    if(lseek(fd, 0, SEEK_SET) < 0) {
        perror("lseek");
        free(buffer);
        return false;
    }

    char headers_dir[PATH_MAX];
    snprintf(headers_dir, sizeof(headers_dir), "%s/headers", chunk_dir);
    if(!ensure_dir(headers_dir)) {
        free(buffer);
        return false;
    }

    const char *base = path_basename(input);
    char header_path[PATH_MAX];
    size_t dir_len = strlen(headers_dir);
    size_t base_len = strlen(base);
    const char suffix[] = ".header";
    if(dir_len + 1 + base_len + sizeof(suffix) > sizeof(header_path)) {
        fprintf(stderr, "Header path for %s is too long\n", input);
        free(buffer);
        return false;
    }
    memcpy(header_path, headers_dir, dir_len);
    header_path[dir_len] = '/';
    memcpy(header_path + dir_len + 1, base, base_len);
    memcpy(header_path + dir_len + 1 + base_len, suffix, sizeof(suffix));

    int out_fd = open(header_path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if(out_fd < 0) {
        perror(header_path);
        free(buffer);
        return false;
    }
    bool ok = write_all(out_fd, buffer, header_size);
    close(out_fd);
    free(buffer);
    if(!ok) {
        fprintf(stderr, "Unable to write header to %s\n", header_path);
        return false;
    }
    return true;
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

static bool ensure_chunk_subdir(const char *chunk_dir, const char *hash_name,
                                const char *prefix, bool uncompressed) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", chunk_dir,
             uncompressed ? "chunks-uncompressed" : "chunks");
    if(!ensure_dir(path))
        return false;
    snprintf(path, sizeof(path), "%s/%s/%s", chunk_dir,
             uncompressed ? "chunks-uncompressed" : "chunks", hash_name);
    if(!ensure_dir(path))
        return false;
    snprintf(path, sizeof(path), "%s/%s/%s/%c%c", chunk_dir,
             uncompressed ? "chunks-uncompressed" : "chunks", hash_name,
             prefix[0], prefix[1]);
    return ensure_dir(path);
}

static bool write_chunk_file(const char *chunk_dir, const char *hash_name,
                             const char *digest, const char *data,
                             size_t data_len, bool uncompressed, bool force) {
    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%s/%s/%s/%c%c", chunk_dir,
             uncompressed ? "chunks-uncompressed" : "chunks", hash_name,
             digest[0], digest[1]);
    if(!ensure_dir(dir_path))
        return false;

    char file_path[PATH_MAX];
    snprintf(file_path, sizeof(file_path), "%s/%s/%s/%c%c/%s.chunk", chunk_dir,
             uncompressed ? "chunks-uncompressed" : "chunks", hash_name,
             digest[0], digest[1], digest);

    int flags = O_WRONLY | O_CREAT | O_BINARY;
    if(force)
        flags |= O_TRUNC;
    else
        flags |= O_EXCL;

    int fd = open(file_path, flags, 0644);
    if(fd < 0) {
        if(errno == EEXIST && !force)
            return true;
        perror(file_path);
        return false;
    }

    bool ok = true;
    if(data_len > 0)
        ok = write_all(fd, data, data_len);
    close(fd);
    if(!ok) {
        fprintf(stderr, "Unable to write %s\n", file_path);
        return false;
    }
    return true;
}

static bool export_chunks(zckCtx *zck, const char *chunk_dir, bool force) {
    const char *hash_name = zck_hash_name_from_type(zck_get_chunk_hash_type(zck));
    char hash_dir[128];
    hash_to_dir_name(hash_name, hash_dir, sizeof(hash_dir));
    ssize_t chunk_count = zck_get_chunk_count(zck);
    if(chunk_count < 0)
        return false;

    for(ssize_t i = 0; i < chunk_count; i++) {
        zckChunk *chunk = zck_get_chunk(zck, i);
        if(!chunk) {
            fprintf(stderr, "Unable to read chunk %lli\n", (long long)i);
            return false;
        }
        ssize_t comp_len = zck_get_chunk_comp_size(chunk);
        if(comp_len < 0)
            return false;
        if(comp_len == 0)
            continue;
        char *digest = zck_get_chunk_digest(chunk);
        bool uncompressed = false;
        if(!digest) {
            fprintf(stderr, "Unable to read chunk digest\n");
            return false;
        }
        if(digest_all_zero(digest)) {
            free(digest);
            digest = zck_get_chunk_digest_uncompressed(chunk);
            uncompressed = true;
            if(!digest) {
                fprintf(stderr, "Chunk missing digest\n");
                return false;
            }
        }

        if(strlen(digest) < 2) {
            fprintf(stderr, "Digest too short\n");
            free(digest);
            return false;
        }
        if(!ensure_chunk_subdir(chunk_dir, hash_dir, digest, uncompressed)) {
            free(digest);
            return false;
        }

        size_t comp_size = (size_t)comp_len;
        char *buffer = malloc(comp_size);
        if(!buffer) {
            fprintf(stderr,
                    "Unable to allocate %lli bytes for chunk export\n",
                    (long long)comp_len);
            free(digest);
            return false;
        }
        ssize_t read_len = zck_get_chunk_comp_data(chunk, buffer, comp_size);
        if(read_len != (ssize_t)comp_size) {
            fprintf(stderr, "Short read exporting chunk\n");
            free(buffer);
            free(digest);
            return false;
        }
        bool ok = write_chunk_file(chunk_dir, hash_dir, digest, buffer, comp_size,
                                   uncompressed, force);
        free(buffer);
        free(digest);
        if(!ok)
            return false;
    }
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
        case 'f':
            arguments->force = true;
            break;
        case 'V':
            version();
            arguments->exit = true;
            break;
        case ARGP_KEY_ARG:
            if (state->arg_num == 0) {
                arguments->chunk_dir = arg;
            } else {
                if(arguments->input_count + 1 >= arguments->input_alloc) {
                    size_t new_alloc = arguments->input_alloc ?
                                       arguments->input_alloc * 2 : 4;
                    char **tmp = realloc(arguments->inputs,
                                         new_alloc * sizeof(char*));
                    if(!tmp)
                        return ENOMEM;
                    arguments->inputs = tmp;
                    arguments->input_alloc = new_alloc;
                }
                arguments->inputs[arguments->input_count++] = arg;
            }
            break;
        case ARGP_KEY_END:
            if(!arguments->chunk_dir || arguments->input_count == 0) {
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

    if(!ensure_dir(arguments.chunk_dir))
        exit(1);

    for(size_t i = 0; i < arguments.input_count; i++) {
        const char *input = arguments.inputs[i];
        int fd = open(input, O_RDONLY | O_BINARY);
        if(fd < 0) {
            perror(input);
            exit(1);
        }

        zckCtx *zck = zck_create();
        if(!zck) {
            close(fd);
            exit(1);
        }
        if(!zck_init_read(zck, fd)) {
            LOG_ERROR("Unable to open %s: %s", input, zck_get_error(zck));
            zck_free(&zck);
            close(fd);
            exit(1);
        }

        if(!write_meta(arguments.chunk_dir, zck, arguments.force)) {
            zck_free(&zck);
            close(fd);
            exit(1);
        }
        if(!export_header(zck, arguments.chunk_dir, input)) {
            zck_free(&zck);
            close(fd);
            exit(1);
        }
        if(!export_chunks(zck, arguments.chunk_dir, arguments.force)) {
            zck_free(&zck);
            close(fd);
            exit(1);
        }

        zck_free(&zck);
        close(fd);
    }

    free(arguments.inputs);
    return 0;
}
