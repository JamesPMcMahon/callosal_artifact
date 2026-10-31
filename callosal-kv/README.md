






# callosal-kv

`callosal-kv` is a small shared-file key-value benchmark built around POSIX `pread()`/`pwrite()`. It is intended for evaluating file systems that expose the same data directory on multiple nodes while allowing concurrent reads and writes to overlapping file regions.

The benchmark has two modes:

- `--populate`: deterministically pre-create the shard files for a given cluster layout.
- `--run`: execute the workload and write per-node JSON statistics.

## Requirements

- C toolchain with `make`, `clang` or `cc`, libc, and pthreads
- Python 3.12+ and `uv` for the multi-node harness under [`harness/`](harness)
- `jq` only if you use [`scripts/sweep.sh`](scripts/sweep.sh)
- Linux is the primary target for benchmark runs; the remote setup script targets Ubuntu 24.04

Ubuntu 24.04 packages for a worker node:

```bash
sudo apt-get update
sudo apt-get install -y build-essential clang-18 clang-tidy-18 clang-format-18 rsync jq python3 python3-venv
curl -LsSf https://astral.sh/uv/install.sh | sh
sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 100
sudo update-alternatives --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-18 100
sudo update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-18 100
```

## Build and Test

```bash
make
make test
make tidy
```

- `make` builds `./callosal-kv`
- `make test` builds and runs the sanitizer-backed test binary
- `make bench` runs the local single-node sweep in [`scripts/sweep.sh`](scripts/sweep.sh)


## Running
NOTE: Callosal-FS requires files to be pre-created, pre-populated, and pre-read to ensure correct size sync when testing multiple nodes. \
This population is done with x\_echo\_files.sh with x being the number of nodes. Then one node should run bash populate.sh populate\_file directory \
populate file has 500 4KB pages worth of text since that is the maximum file size we tested with. \
Then for each node run cat directory/* \
now the sizes should be properly synced. Otherwise there might be issues that come up \ 
(these files are present in the Callosal-FS section of the repository) \ 


now the benchmark can be run with: sudo bash dfs\_run\_script.sh (index 0-3) (total 1-4) (duration seconds) (this IP address) (number of thread combinations) 239400 500 \n
Similarly, NFS and CephFS are tested with: run\_nfs.sh and run\_script.sh respectively \
These scripts can be configured for the distribution type (zipf uniform) and write percentage \

python\_server.py is used for thread count distribution and syncing over UDP \
(present in the sync\_scripts directory as well)\

