#include <argp.h>
#include <ctype.h>
#include <dirent.h>
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

#include <zck.h>

#include "util_common.h"

#if defined(ZCHUNK_OPENSSL) && !defined(ZCHUNK_OPENSSL_DEPRECATED)
#include <openssl/evp.h>
#else
#include "lib/hash/bundled/sha1/sha1.h"
#include "lib/hash/bundled/sha2/sha2.h"
#endif

static char doc[] =
    "zck_chunk_validate - Validate exported zchunk chunk files";

static char args_doc[] = "<chunk directory>";

struct arguments {
    char *args[1];
    zck_log_type log_level;
    bool exit;
};

static struct argp_option options[] = {
    {"verbose", 'v', 0, 0, "Increase verbosity"},
    {"quiet", 'q', 0, 0,
     "Only show warnings (can be specified twice to only show errors)"},
    {"version", 'V', 0, 0, "Show program version"},
    {0},
};

struct stats {
    size_t files;
    size_t valid;
    size_t zero;
    size_t digest_errors;
    size_t io_errors;
    size_t name_errors;
    size_t size_errors;
};

struct digest_ctx {
    int type;
    size_t digest_size;
#if defined(ZCHUNK_OPENSSL) && !defined(ZCHUNK_OPENSSL_DEPRECATED)
    EVP_MD_CTX *ctx;
#else
    union {
        SHA_CTX sha1;
        sha256_ctx sha256;
        sha512_ctx sha512;
    } state;
#endif
};

static size_t digest_size_for_type(int hash_type) {
    switch(hash_type) {
        case ZCK_HASH_SHA1:
            return 20;
        case ZCK_HASH_SHA256:
            return 32;
        case ZCK_HASH_SHA512:
            return 64;
        case ZCK_HASH_SHA512_128:
            return 16;
        default:
            return 0;
    }
}

#if defined(ZCHUNK_OPENSSL) && !defined(ZCHUNK_OPENSSL_DEPRECATED)
static const EVP_MD *digest_evp_for_type(int hash_type) {
    switch(hash_type) {
        case ZCK_HASH_SHA1:
            return EVP_sha1();
        case ZCK_HASH_SHA256:
            return EVP_sha256();
        case ZCK_HASH_SHA512:
        case ZCK_HASH_SHA512_128:
            return EVP_sha512();
        default:
            return NULL;
    }
}
#endif

static bool digest_ctx_init(struct digest_ctx *ctx, int hash_type) {
    ctx->type = hash_type;
    ctx->digest_size = digest_size_for_type(hash_type);
    if(ctx->digest_size == 0)
        return false;
#if defined(ZCHUNK_OPENSSL) && !defined(ZCHUNK_OPENSSL_DEPRECATED)
    const EVP_MD *md = digest_evp_for_type(hash_type);
    if(md == NULL)
        return false;
    ctx->ctx = EVP_MD_CTX_new();
    if(ctx->ctx == NULL)
        return false;
    if(EVP_DigestInit_ex(ctx->ctx, md, NULL) != 1) {
        EVP_MD_CTX_free(ctx->ctx);
        ctx->ctx = NULL;
        return false;
    }
#else
    switch(hash_type) {
        case ZCK_HASH_SHA1:
            SHA1_Init(&ctx->state.sha1);
            break;
        case ZCK_HASH_SHA256:
            sha256_init(&ctx->state.sha256);
            break;
        case ZCK_HASH_SHA512:
        case ZCK_HASH_SHA512_128:
            sha512_init(&ctx->state.sha512);
            break;
        default:
            return false;
    }
#endif
    return true;
}

static void digest_ctx_cleanup(struct digest_ctx *ctx) {
#if defined(ZCHUNK_OPENSSL) && !defined(ZCHUNK_OPENSSL_DEPRECATED)
    if(ctx->ctx)
        EVP_MD_CTX_free(ctx->ctx);
    ctx->ctx = NULL;
#else
    (void)ctx;
#endif
}

static bool digest_ctx_update(struct digest_ctx *ctx, const unsigned char *data,
                              size_t len) {
#if defined(ZCHUNK_OPENSSL) && !defined(ZCHUNK_OPENSSL_DEPRECATED)
    if(EVP_DigestUpdate(ctx->ctx, data, len) != 1)
        return false;
#else
    switch(ctx->type) {
        case ZCK_HASH_SHA1:
            SHA1_Update(&ctx->state.sha1, data, (unsigned int)len);
            break;
        case ZCK_HASH_SHA256:
            sha256_update(&ctx->state.sha256, data, (unsigned int)len);
            break;
        case ZCK_HASH_SHA512:
        case ZCK_HASH_SHA512_128:
            sha512_update(&ctx->state.sha512, data, (unsigned int)len);
            break;
        default:
            return false;
    }
#endif
    return true;
}

static bool digest_ctx_finalize(struct digest_ctx *ctx, unsigned char **out) {
    size_t digest_len = ctx->digest_size;
    const unsigned char *digest_src = NULL;
#if defined(ZCHUNK_OPENSSL) && !defined(ZCHUNK_OPENSSL_DEPRECATED)
    unsigned int result_len = 0;
    unsigned char temp[EVP_MAX_MD_SIZE];
    if(EVP_DigestFinal_ex(ctx->ctx, temp, &result_len) != 1) {
        digest_ctx_cleanup(ctx);
        return false;
    }
    digest_ctx_cleanup(ctx);
    if(ctx->type == ZCK_HASH_SHA512_128) {
        if(result_len < 16)
            return false;
        digest_len = 16;
    } else if(result_len < digest_len) {
        return false;
    }
    digest_src = temp;
#else
    unsigned char temp[SHA512_DIGEST_SIZE];
    switch(ctx->type) {
        case ZCK_HASH_SHA1:
            SHA1_Final(temp, &ctx->state.sha1);
            break;
        case ZCK_HASH_SHA256:
            sha256_final(&ctx->state.sha256, temp);
            break;
        case ZCK_HASH_SHA512:
            sha512_final(&ctx->state.sha512, temp);
            break;
        case ZCK_HASH_SHA512_128:
            sha512_final(&ctx->state.sha512, temp);
            digest_len = 16;
            break;
        default:
            return false;
    }
    digest_src = temp;
#endif
    unsigned char *buffer = calloc(digest_len, 1);
    if(buffer == NULL)
        return false;
    memcpy(buffer, digest_src, digest_len);
    *out = buffer;
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

static int hash_type_from_name(const char *name) {
    for(int type = ZCK_HASH_SHA1; type < ZCK_HASH_UNKNOWN; type++) {
        const char *candidate = zck_hash_name_from_type(type);
        if(candidate && strcmp(candidate, name) == 0)
            return type;
    }
    return -1;
}

static bool ends_with(const char *name, const char *suffix) {
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);
    if(name_len < suffix_len)
        return false;
    return strcmp(name + name_len - suffix_len, suffix) == 0;
}

static bool is_hex_string(const char *s) {
    for(const char *p = s; *p; p++) {
        if(!isxdigit((unsigned char)*p))
            return false;
    }
    return true;
}

static bool is_zero_digest(const char *digest) {
    for(const char *p = digest; *p; p++) {
        if(*p != '0')
            return false;
    }
    return true;
}

static char *join2(const char *a, const char *b) {
    size_t len = strlen(a) + strlen(b) + 2;
    char *path = calloc(len, 1);
    if(path == NULL)
        return NULL;
    snprintf(path, len, "%s/%s", a, b);
    return path;
}

static char *join3(const char *a, const char *b, const char *c) {
    size_t len = strlen(a) + strlen(b) + strlen(c) + 3;
    char *path = calloc(len, 1);
    if(path == NULL)
        return NULL;
    snprintf(path, len, "%s/%s/%s", a, b, c);
    return path;
}

static char *join4(const char *a, const char *b, const char *c, const char *d) {
    size_t len = strlen(a) + strlen(b) + strlen(c) + strlen(d) + 4;
    char *path = calloc(len, 1);
    if(path == NULL)
        return NULL;
    snprintf(path, len, "%s/%s/%s/%s", a, b, c, d);
    return path;
}

static char *dup_prefix(const char *src, size_t len) {
    char *out = calloc(len + 1, 1);
    if(out == NULL)
        return NULL;
    memcpy(out, src, len);
    return out;
}

static char *digest_to_hex(const unsigned char *digest, size_t length) {
    char *hex = calloc(length * 2 + 1, 1);
    if(hex == NULL)
        return NULL;
    for(size_t i = 0; i < length; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return hex;
}

static bool validate_chunk_file(const char *path, const char *digest,
                                int hash_type, struct stats *stats) {
    stats->files++;

    struct stat st = {0};
    if(stat(path, &st) < 0) {
        LOG_ERROR("Unable to stat %s: %s\n", path, strerror(errno));
        stats->io_errors++;
        return false;
    }

    size_t digest_bytes = digest_size_for_type(hash_type);
    if(digest_bytes == 0) {
        LOG_ERROR("Unsupported hash type for %s\n", path);
        stats->name_errors++;
        return false;
    }

    size_t expected_len = digest_bytes * 2;
    if(strlen(digest) != expected_len || !is_hex_string(digest)) {
        LOG_ERROR("Invalid chunk name %s (expected %zu hex characters)\n",
                  digest, expected_len);
        stats->name_errors++;
        return false;
    }

    if(is_zero_digest(digest)) {
        if(st.st_size != 0) {
            LOG_ERROR("Chunk %s has zero digest but non-zero length\n", path);
            stats->size_errors++;
            return false;
        }
        stats->zero++;
        stats->valid++;
        return true;
    }

    int fd = open(path, O_RDONLY | O_BINARY);
    if(fd < 0) {
        LOG_ERROR("Unable to open %s: %s\n", path, strerror(errno));
        stats->io_errors++;
        return false;
    }

    struct digest_ctx ctx = {0};
    if(!digest_ctx_init(&ctx, hash_type)) {
        LOG_ERROR("Unable to initialize digest context for %s\n", path);
        close(fd);
        stats->io_errors++;
        return false;
    }

    bool error = false;
    unsigned char buffer[BUF_SIZE];
    while(true) {
        ssize_t rb = read(fd, buffer, sizeof(buffer));
        if(rb < 0) {
            LOG_ERROR("Error reading %s: %s\n", path, strerror(errno));
            error = true;
            break;
        }
        if(rb == 0)
            break;
        if(!digest_ctx_update(&ctx, buffer, (size_t)rb)) {
            LOG_ERROR("Unable to update digest for %s\n", path);
            error = true;
            break;
        }
    }

    close(fd);

    if(error) {
        digest_ctx_cleanup(&ctx);
        stats->io_errors++;
        return false;
    }

    unsigned char *digest_raw = NULL;
    if(!digest_ctx_finalize(&ctx, &digest_raw)) {
        stats->io_errors++;
        return false;
    }

    char *digest_hex = digest_to_hex(digest_raw, digest_bytes);
    free(digest_raw);
    if(digest_hex == NULL) {
        stats->io_errors++;
        return false;
    }

    bool match = strcmp(digest_hex, digest) == 0;
    if(!match) {
        LOG_ERROR("Digest mismatch for %s\n", path);
        stats->digest_errors++;
    } else {
        stats->valid++;
    }

    free(digest_hex);
    return match;
}

static bool validate_prefix_dir(const char *base, const char *hash_name,
                                const char *prefix, int hash_type,
                                struct stats *stats) {
    char *prefix_path = join3(base, hash_name, prefix);
    if(prefix_path == NULL)
        return false;

    DIR *dir = opendir(prefix_path);
    if(dir == NULL) {
        LOG_ERROR("Unable to open %s: %s\n", prefix_path, strerror(errno));
        free(prefix_path);
        return false;
    }

    bool ok = true;
    struct dirent *entry = NULL;
    while((entry = readdir(dir)) != NULL) {
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if(!ends_with(entry->d_name, ".chunk")) {
            LOG_ERROR("Skipping unexpected file %s/%s\n", prefix_path,
                      entry->d_name);
            stats->name_errors++;
            ok = false;
            continue;
        }
        size_t name_len = strlen(entry->d_name);
        size_t digest_len = name_len - strlen(".chunk");
        char *digest = dup_prefix(entry->d_name, digest_len);
        if(digest == NULL) {
            ok = false;
            break;
        }
        char *chunk_path = join4(base, hash_name, prefix, entry->d_name);
        if(chunk_path == NULL) {
            free(digest);
            ok = false;
            break;
        }
        bool chunk_ok = validate_chunk_file(chunk_path, digest, hash_type, stats);
        if(!chunk_ok)
            ok = false;
        free(chunk_path);
        free(digest);
    }

    closedir(dir);
    free(prefix_path);
    return ok;
}

static bool validate_hash_dir(const char *base, const char *hash_name,
                              struct stats *stats) {
    int hash_type_id = hash_type_from_name(hash_name);
    if(hash_type_id < 0) {
        LOG_ERROR("Skipping unknown hash directory %s\n", hash_name);
        stats->name_errors++;
        return false;
    }

    char *hash_path = join2(base, hash_name);
    if(hash_path == NULL)
        return false;

    DIR *dir = opendir(hash_path);
    if(dir == NULL) {
        LOG_ERROR("Unable to open %s: %s\n", hash_path, strerror(errno));
        free(hash_path);
        return false;
    }

    bool ok = true;
    struct dirent *entry = NULL;
    while((entry = readdir(dir)) != NULL) {
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char *prefix_path = join3(base, hash_name, entry->d_name);
        if(prefix_path == NULL) {
            ok = false;
            break;
        }
        free(prefix_path);
        if(!validate_prefix_dir(base, hash_name, entry->d_name, hash_type_id,
                                 stats))
            ok = false;
    }

    closedir(dir);
    free(hash_path);
    return ok;
}

int main(int argc, char *argv[]) {
    struct arguments arguments = {0};
    arguments.log_level = ZCK_LOG_INFO;

    int retval = argp_parse(&argp, argc, argv, 0, 0, &arguments);
    if(retval || arguments.exit)
        exit(retval);

    zck_set_log_level(arguments.log_level);

    const char *chunk_dir = arguments.args[0];
    DIR *base_dir = opendir(chunk_dir);
    if(base_dir == NULL) {
        LOG_ERROR("Unable to open %s: %s\n", chunk_dir, strerror(errno));
        exit(10);
    }

    struct stats stats = {0};
    bool ok = true;
    struct dirent *entry = NULL;
    while((entry = readdir(base_dir)) != NULL) {
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char *hash_path = join2(chunk_dir, entry->d_name);
        if(hash_path == NULL) {
            ok = false;
            break;
        }
        struct stat st = {0};
        if(stat(hash_path, &st) < 0) {
            LOG_ERROR("Unable to stat %s: %s\n", hash_path, strerror(errno));
            free(hash_path);
            stats.io_errors++;
            ok = false;
            continue;
        }
        free(hash_path);
        if(!S_ISDIR(st.st_mode))
            continue;
        if(!validate_hash_dir(chunk_dir, entry->d_name, &stats))
            ok = false;
    }

    closedir(base_dir);

    printf("Validated %llu chunk files: %llu ok, %llu zero-length",
           (long long unsigned)stats.files,
           (long long unsigned)stats.valid,
           (long long unsigned)stats.zero);
    if(stats.digest_errors)
        printf(", %llu digest mismatches",
               (long long unsigned)stats.digest_errors);
    if(stats.size_errors)
        printf(", %llu size errors",
               (long long unsigned)stats.size_errors);
    if(stats.name_errors)
        printf(", %llu naming issues",
               (long long unsigned)stats.name_errors);
    if(stats.io_errors)
        printf(", %llu I/O errors",
               (long long unsigned)stats.io_errors);
    printf("\n");

    if(stats.digest_errors || stats.io_errors || stats.name_errors ||
       stats.size_errors)
        return 1;
    if(!ok)
        return 1;
    return 0;
}
