# Offline updates with zck_chunk_store and zckdir

The `zck_chunk_store` utility exports the compressed chunks from one or more
`.zck` files into a filesystem cache. `zckdir` consumes that cache to rebuild a
`.zck` file without fetching data from an HTTP server.

## Chunk cache layout

Running `zck_chunk_store /path/to/cache file.zck` creates the following
structure:

```
/path/to/cache/
├── headers/
│   └── file.zck.header        # lead + header bytes to seed clients
├── meta.conf                  # chunk hash metadata
└── chunks/
    └── <hash-name>/
        ├── <hh>/<digest>.chunk
        └── ...
```

* `meta.conf` records the chunk hash algorithm and digest size used by the
  cached files. `zckdir` refuses to use a cache whose metadata does not match
  the target file.
* `headers/` contains byte-for-byte copies of the lead and header for each
  exported `.zck` file. Copy one of these header files to the destination path
  before invoking `zckdir`, or pass it via `--header` and let the tool copy it
  for you.
* Chunk data files live under `chunks/<hash-name>/<hh>/<digest>.chunk`, where
  `<hash-name>` is the chunk hash name (e.g. `sha512_128`), `<hh>` is the first
  two hex characters of the digest, and the filename is the full digest. If a
  chunk has a compressed digest of all zeros the uncompressed digest is used and
  the chunk is stored under `chunks-uncompressed/<hash-name>/...` instead.

Re-running `zck_chunk_store` against additional `.zck` files deduplicates
matching chunks automatically because the digest-based filenames collide.

## Populating removable media

To prepare a cache for transfer:

```bash
$ mkdir -p /srv/zck-cache
$ zck_chunk_store /srv/zck-cache path/to/a.zck path/to/b.zck
$ rsync -av /srv/zck-cache/ /media/usb/zck-cache/
```

Only the subset of `*.chunk` files that are missing on the client side need to
be copied to removable media for subsequent updates.

## Rebuilding a file offline

1. Copy the matching header file to the target path (or rely on `--header`).
2. Mount or copy the cache directory to the client.
3. Run `zckdir --chunk-dir /media/usb/zck-cache --header /media/usb/zck-cache/headers/file.zck.header file.zck`.

`zckdir` validates any existing data, copies matching chunks from a local source
file when `--source` is provided, and imports chunk files from the cache. When
all chunks are present it truncates the output to the expected length and
verifies the overall checksum.

If some digests are still missing, `zckdir` lists them so that the operator can
transfer only the required `*.chunk` files during the next update cycle.
