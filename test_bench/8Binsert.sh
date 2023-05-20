sudo -S bash -c 'echo 800000 > /proc/sys/fs/file-max'
ulimit -n 800000

rm -rf /mnt/shunzi/hjx/*
rm  /home/junix/log/*.log
sudo -S fstrim /home/junix/log
sudo -S fstrim /mnt/shunzi/hjx
sudo -S bash -c 'echo 1 > /proc/sys/vm/drop_caches'
cgexec -g memory:kv64 ./kv/release/tests/db/kv_bench --hugepage=true --db=/mnt/shunzi/hjx/kv8B  --threads=1 --num=800000000 --value_size=100 --batch_size=1000 --benchmarks=fillrandom,stats --logpath=/home/junix/log --bloom_bits=10 --log=true        --cache_size=8388608 --low_pool=3 --high_pool=3 --open_files=40000 --stats_interval=100000000 --histogram=true --compression=0     --write_buffer_size=2097152 --skiplistrep=false --log_dio=true  --partition=10  --print_wa=true | tee kv8B_nvm_hugepage.log 

rm -rf /mnt/shunzi/hjx/*
rm  /home/junix/log/*.log
sudo -S fstrim /home/junix/log
sudo -S fstrim /mnt/shunzi/hjx
sudo -S bash -c 'echo 1 > /proc/sys/vm/drop_caches'
cgexec -g memory:kv64 ./rocksdb/release/db_bench  --db=/mnt/shunzi/hjx/rocks8B  --num=800000000 --value_size=100 --batch_size=1000  --benchmarks=fillrandom,stats --wal_dir=/home/junix/log --bloom_bits=10 --disable_wal=false --cache_size=8388608    --max_background_jobs=7 --open_files=40000 --stats_per_interval=100000000  --stats_interval=100000000 --histogram=true  | tee rocks8B_nvm.log


rm -rf /mnt/shunzi/hjx/*
rm  /home/junix/log/*.log
sudo -S fstrim /home/junix/log
sudo -S fstrim /mnt/shunzi/hjx
sudo -S bash -c 'echo 1 > /proc/sys/vm/drop_caches'
cgexec -g memory:kv64 ./pebblesdb/release/db_bench  --db=/mnt/shunzi/hjx/peb8B  --num=8000000000 --value_size=100 --batch_size=1000  --benchmarks=fillrandom,stats --logpath=/home/junix/log --bloom_bits=10 --log=true       --cache_size=8388608     --bg_threads=6         --open_files=800000 --stats_interval=100000000 --histogram=true --compression=false --write_buffer_size=67108864   | tee peb8B_nvm.log


rm -rf /mnt/shunzi/hjx/*
rm  /home/junix/log/*.log
sudo -S fstrim /home/junix/log
sudo -S fstrim /mnt/shunzi/hjx
sudo -S bash -c 'echo 1 > /proc/sys/vm/drop_caches'
cgexec -g memory:kv64 ./leveldb/release/db_bench  --db=/mnt/shunzi/hjx/level8B  --num=8000000000 --value_size=100 --batch_size=1000 --benchmarks=fillrandom,stats --logpath=/home/junix/log --bloom_bits=10 --log=1       --cache_size=8388608                            --open_files=40000  --stats_interval=100000000  --histogram=1                        --write_buffer_size=67108864  --max_file_size=67108864   --print_wa=true | tee level8B_nvm.log

