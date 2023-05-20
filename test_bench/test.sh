#!/bin/bash
set -x
export LC_ALL=C

# mkdir /sys/fs/cgroup/memory/kv64
# echo 64G > /sys/fs/cgroup/memory/kv64/memory.limit_in_bytes
# echo 0 > /sys/fs/cgroup/memory/kv64/memory.swappiness

echo 16384 > /proc/sys/vm/nr_hugepages
sudo -S bash -c 'echo 800000 > /proc/sys/fs/file-max'
ulimit -n 800000

op_num=800000000
duration=0
user_thread_num=1
high_thread_num=3
low_thread_num=3
value_size=100
write_buffer_size=2
max_file_size=1024
monitor_interval=1
db_dir="/mnt/hjx/hjx"
wal_dir="/mnt/sdb/hjx"
db_disk="nvme0n1"
wal_disk="sdb"
batch_size=1000
partition=1

direct=true
log_direct=true
log=true


function db_bench() {
    rm ./exp_*
    rm -rf ${db_dir}/*
    rm ${wal_dir}/*
    sudo -S fstrim ${db_dir}
    sudo -S fstrim ${wal_dir}
    sync; echo 3 > /proc/sys/vm/drop_caches
    
    # cgexec -g memory:kv64 \
    ../kv/release/tests/db/kv_bench \
    --hugepage=true  \
    --db=${db_dir}/wipdb  \
    --threads=${user_thread_num} \
    --num=$((${op_num} / ${user_thread_num})) \
    --value_size=${value_size} \
    --batch_size=${batch_size} \
    --benchmarks=fillrandom,stats \
    --logpath=${wal_dir} \
    --log=${log} \
    --low_pool=${low_thread_num} --high_pool=${high_thread_num} \
    --stats_interval=100000000 --histogram=true \
    --compression=0 \
    --write_buffer_size=$((${write_buffer_size} << 20)) \
    --skiplistrep=false \
    --direct_io=${direct} \
    --log_dio=${log_direct}  \
    --partition=${partition} > exp_result.log &

    # --open_files=40000 \
    # --bloom_bits=10 \
    # --cache_size=8388608 \


    db_bench_pid=$!

    top -H -b d ${monitor_interval} -p ${db_bench_pid} >exp_top_raw.txt &
    # top -H -b d ${monitor_interval} | grep -e 'db_bench' -e 'kv_bench' -e 'rocksdb:' > exp_top.txt &
    top_pid=$!
    iostat -mtx ${monitor_interval} >exp_iostat_raw.txt &
    # iostat -mtx ${monitor_interval} | grep -e ${db_disk} -e ${wal_disk} > exp_iostat.txt &
    iostat_pid=$!

    pidstat -p ${db_bench_pid} -d -t ${monitor_interval} >exp_pidstat_io_raw.txt &
    # pidstat -p ${db_bench_pid} -dRsuvr -H -t 1 > exp_pidstat.txt&
    pidstat_io_pid=$!

    pidstat -p ${db_bench_pid} -u -t ${monitor_interval} >exp_pidstat_cpu_raw.txt &
    pidstat_cpu_pid=$!

    # fg $(jobs | grep "db_bench" | awk '{print $1}')
    wait ${db_bench_pid}

    sync
    kill ${top_pid}
    kill ${iostat_pid}
    kill ${pidstat_io_pid}
    kill ${pidstat_cpu_pid}

    grep -e 'db_bench' -e 'kv_bench' -e 'rocksdb:' exp_top_raw.txt >exp_top.txt
    grep -E '^top' exp_top_raw.txt | awk '{print $3}' >exp_top_time.txt
    grep -e ${db_disk} -e ${wal_disk} exp_iostat_raw.txt >exp_iostat.txt
    grep '^[0-9][0-9]/[0-9][0-9]/[0-9][0-9]' exp_iostat_raw.txt | cut -d ' ' -f 2 >exp_iostat_time.txt
    grep -e '|__' exp_pidstat_io_raw.txt >exp_pidstat_io.txt
    # grep '^\ \ L[0-9].*' exp_op_data | awk '{print $1,$5}' >exp_compaction_score

    cur_sec=$(date '+%s')
    result_dir=${cur_sec}_wipdb_result_${op_num}_${value_size}_${write_buffer_size}M_${user_thread_num}user_${high_thread_num}high_${low_thread_num}low_${partition}partition

    rm -rf ./data/${result_dir}

    mkdir ./data/${result_dir}

    # perf script -i exp_perf.data > exp_out.perf
    # ../FlameGraph/stackcollapse-perf.pl exp_out.perf > exp_out.folded
    # ../FlameGraph/flamegraph.pl exp_out.folded > exp_perf.svg

    mv ./exp* ./data/${result_dir}
    mv ${db_dir}/wipdb/LOG ./data/${result_dir}
}

log_direct=false
db_bench



value_size=1000
batch_size=1
duration=3600
log_direct=true
db_bench