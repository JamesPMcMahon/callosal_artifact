
# Callosal Artifacts
This repository contains the following modules:
1. A filesystem using Callosal's page cache sync
2. Modified version of [MIND kernel](https://github.com/JamesPMcMahon/callosal_artifact/tree/main/mind)
3. Modified version of fxmark for multi-node setup
4. Callosal ring buffer implementation
5. Callosal key-value store implementation

# Warning:
The code in this repository is included for purposes of artifact availability. The Callosal code relies on specific machine configurations and code that is subject to an NDA. Further, certain parts of this artifact have been scrubbed of specific IP addresses, MAC addresses, users, and directories. This artifact is built with the assumption that the correct machine configuration and additional code are available/restored.

### Hardware Requirements
- Default cluster environment consists of 4 compute blade VMs, 1 memory blade VM, and 1 programmable switch. We used a 100 Gbps link for all connections between VMs and the switch; VMs are distributed over 2 compute servers and 1 memory server.
- We enabled IO-MMU and used PCIe passthrough to attach NIC to VMs.

## Artifact Usage Instructions

## Filesystem code
The [filesystem](https://github.com/JamesPMcMahon/callosal_artifact/tree/main/callosal_fs) source code is included; we ran it on a modified version of the 4.15 kernel with the MIND subsystem built in. 

### How to build MIND Kernel for Callosal (adapted from [MIND Kernel](https://github.com/yale-nova/mind/tree/main))
- Please find the link to each system component below.
1. Modified [MIND kernel](https://github.com/JamesPMcMahon/callosal_artifact/tree/main/mind) for Callosal
2. Switch program
    - [Dependencies](https://github.com/JamesPMcMahon/callosal_artifact/blob/main/mind/mind_switch_ctrl/dependencies.md)
    - [Control plane C/C++ program](https://github.com/JamesPMcMahon/callosal_artifact/tree/main/mind/mind_switch_ctrl)
    - [Data plane P4 program](https://github.com/JamesPMcMahon/callosal_artifact/tree/main/mind/mind_p4)
  
### How to build and use the CallosalFS module
- Please find the link to each system component below.
1. [CallosalFS](https://github.com/4rnge/Callosal_artifacts/tree/main/callosal_fs) module
2. Clone the [CallosalFS](https://github.com/4rnge/Callosal_artifacts/tree/main/callosal_fs) module within the linux/fs/ directory
3. To mount the module on 4 compute blade VMs
   ```
   sudo insmod simplefs.ko [first node]
   sudo insmod simplefs.ko readAddress=1 combined_address=-25289002319872 [other 3 nodes]
   ```
   Note: (combined address taken from `dmesg`: the first address after the printout "addresses" after the size is printed)
   (readAddress=1 is required for the module to read address from command line)
   
4. Generate 'test' image by creating a zeroed file of 50 MiB. We can then mount this image on a system with the Callosal kernel module installed.
   ```
   mkdir -p test
   dd if=/dev/zero of=test.img bs=1M count=50
   ./mkfs.simplefs test.img
   sudo mount -o loop -t simplefs test.img test
   ```
5. Run tests within the 'test' directory. 
4. Remove the kernel mount point and module:
   ```
   sudo umount test
   sudo rmmod simplefs
   ```

## Running FxMark benchmarks and applications

### Modified FxMark
We included a modified version of FxMark for testing; the changes primarily support synchronization across multiple nodes. 

#### Running FxMark across the separate systems
1. Clone the [FxMark](https://github.com/JamesPMcMahon/callosal_artifact/tree/main/fxmark_synced) and run 'make'.
2. Inside the bin directory, there are 3 wrapper scripts: one for CallosalFS, NFS, and CephFS.
    ```
        sudo bash wrapper_dfs.sh #CallosalFS
        sudo bash wrapper_nfs.sh #NFS
        sudo bash wrapper_script.sh #CephFS
    ```
3. Inside the wrapper scripts, the index and offset variables at the top will need to be modified. There are 4 nodes, each with 6 files/pages synced, so the indexes are "1,2,3,4" and the offset is "0,6,12,18" for nodes 1,2,3, and 4.
4. Next, inside the wrapper scripts, there is a for loop over the benchmarks that will be run.
5. At the beginning of each benchmark, one node should run the benchmark for a short period with the "setup" flag set to 1. This is done to avoid performance issues with CephFS.
6. Next, each script runs another script, for example: 'run_nfs.sh'. This script loops until it receives thread count 9, marking the end of the benchmark.
7. The 'run' scripts are syncing with ```python_start.py``` which waits for a ```python_server.py``` script to inform it of what threads to run.
8. These wrapper scripts should be started on each machine, wait for the warmup/setup to finish, and then run ```python_server.py```. with the desired thread counts configured, ending each benchmark by sending 9 as the thread count.
9. NOTE: CallosalFS can run out of files, so it will need to be restarted between benchmark runs.
10. To test with different read/write percentages like on DRWOH and DRWOM, change the value that 'toss' is compared to in the DRWOM.c and DRWOM.c files and re-make
    
### Python UDP sync
FxMark and the Key Value Store sync starts and thread counts using a ```python_server.py``` script available in the sync scripts section.

```python3 python_server.py benchmark_time_seconds IP1 IP2 IP3 IP4```

Specific thread counts and ordering are broken down in the threadcounts variable in this file

### Callosal Ring Buffer 
We included a simple benchmark that uses files to create a ring buffer to send data from one node to another.
This benchmark relies on cross-node file-synchronization correctness, and tests performance.

1. There are two files, one for CN1 and one for CN2.
2. Inside these files, there are configurable buffersize and write_count variables.
3. we built the writer and the reader using ```gcc -O3 -Wall -g```.
4. For a directory 'test' we place the runscripts in the directory above it and run it from within the 'test' directory.
5. ``` sudo ../writer.out ``` which will create two files: 0_0_buffer and 0_0_metadata.
6. The reader can then be run using the same method on the other machine (after the writer has begun).
7. NOTE: On CallosalFS, the files should be pre-created on both nodes.
8. The output reports the performance results.

### Callosal Key Value Store
This tests a version of an exclusive-writer, multiple-reader key-value store that synchronizes state through the filesystem. 

#### Requirements

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

#### Build and Test

```bash make
```

To run our tests, 
1. In the harness folder, there are the scripts: dfs_run_script.sh new_nfs_script.sh and run_script.sh used for CallosalFS, NFS, and CephFS.
2. In the scripts, the distributions and read/write ratio can be configured by changing ```dists=("zipf" "uniform")``` and ```ratios=("0.0" "0.01" "0.05")```.
3. The directory can be configured within the script as well using the ```--path argument within the script for the callosal-kv execution```.
4. To run the scripts across nodes use: ```sudo bash dfs_run_script.sh (nodeid 0-3) (total nodes 1-4) (time seconds) (this IP address) (number of thread combinations per config) 239400 500```.
5. Thread counts and start times are synced using ```python_server.py```; thread count 9 doesn't auto-end the benchmark.
6. The results are printed on stdout. Adding them up gives the results that we reported. 

NOTE: Callosal-FS requires files to be pre-created, pre-populated, and pre-read to ensure correct size sync when testing multiple nodes.
This population is done with x\_echo\_files.sh, with x being the number of nodes. Then one node should run bash populate.sh populate\_file directory. The populate file has 500 4KB pages of text, since that is the maximum file size we tested with.
Then, for each node, run cat directory. Now the sizes should be properly synced. Otherwise, there might be issues that come up (these files are present in the Callosal-FS section of the repository). 

Callosal-FS steps:
1. Install the Callosal-FS module on nodes
2. run ``sudo bash ../x_echo_files.sh`` on both nodes for x number of nodes within the test directory
3. On one node, run ```sudo bash populate.sh 500_page_file test``` 
4. Run cat test/* on all other nodes
5. run the dfs_run_script ```sudo bash dfs_run_script.sh this_node total_nodes duration this_ip run_count 239400 500```
6. run the ```python3 python_server.py duration IP1 IP2 IP3 IP4``` with the configured thread count/order 
7. Add up the output results


Similarly, NFS and CephFS are tested with: run\_nfs.sh and run\_script.sh respectively.
These scripts can be configured for the distribution type (zipf uniform) and write percentage.

`python_server.py` is used for thread count distribution and syncing over UDP (present in the sync\_scripts directory as well).


