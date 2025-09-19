subdir('lib')

preprocessor_defines = []
if host_machine.system() == 'windows'
    extra_win_src = ['lib/win32/basename.c', 'lib/win32/ftruncate.c']
    if get_option('default_library') == 'static'
        preprocessor_defines = '-DZCHUNK_STATIC_LIB=1'
    endif
else
    extra_win_src = []
endif

zck = executable(
    'zck',
    ['zck.c', 'util_common.c', 'memmem.c'] + extra_win_src,
    include_directories: inc,
    dependencies: argplib,
    link_with: zcklib,
    install: true,
    c_args: preprocessor_defines
)
unzck = executable(
    'unzck',
    ['unzck.c', 'util_common.c'] + extra_win_src,
    include_directories: inc,
    dependencies: argplib,
    link_with: zcklib,
    install: true,
    c_args: preprocessor_defines
)
zck_gen_zdict = executable(
    'zck_gen_zdict',
    ['zck_gen_zdict.c', 'util_common.c'] + extra_win_src,
    include_directories: inc,
    dependencies: argplib,
    link_with: zcklib,
    install: true,
    c_args: preprocessor_defines
)
zck_read_header = executable(
    'zck_read_header',
    ['zck_read_header.c', 'util_common.c'] + extra_win_src,
    include_directories: inc,
    dependencies: argplib,
    link_with: zcklib,
    install: true,
    c_args: preprocessor_defines
)
zck_delta_size = executable(
    'zck_delta_size',
    ['zck_delta_size.c', 'util_common.c', 'memmem.c'] + extra_win_src,
    include_directories: inc,
    dependencies: argplib,
    link_with: zcklib,
    install: true,
    c_args: preprocessor_defines
)
zckdl = executable(
    'zckdl',
    ['zck_dl.c', 'util_common.c'] + extra_win_src,
    include_directories: inc,
    dependencies: [argplib, curl_dep],
    link_with: zcklib,
    install: true,
    c_args: preprocessor_defines
)
zck_chunk_store = executable(
    'zck_chunk_store',
    ['zck_chunk_store.c', 'util_common.c'] + extra_win_src,
    include_directories: inc,
    dependencies: argplib,
    link_with: zcklib,
    install: true,
    c_args: preprocessor_defines
)
chunk_validate_sources = ['zck_chunk_validate.c', 'util_common.c']
chunk_validate_deps = [argplib]
if not openssl_dep.found()
    chunk_validate_sources += [
        'lib/hash/bundled/sha1/sha1.c',
        'lib/hash/bundled/sha2/sha2.c',
    ]
else
    chunk_validate_deps += openssl_dep
endif
zck_chunk_validate = executable(
    'zck_chunk_validate',
    chunk_validate_sources + extra_win_src,
    include_directories: inc,
    dependencies: chunk_validate_deps,
    link_with: zcklib,
    install: true,
    c_args: preprocessor_defines
)
zckdlfs = executable(
    'zckdlfs',
    ['zck_dl_dir.c', 'util_common.c'] + extra_win_src,
    include_directories: inc,
    dependencies: argplib,
    link_with: zcklib,
    install: true,
    c_args: preprocessor_defines
)
