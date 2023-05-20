#pragma once

#include <stdarg.h>

#include <mutex>
#include <string>
#define STATISTIC_OPEN

#define LZW_INFO
#ifdef LZW_INFO
#define RECORD_INFO(file_num, format, ...) \
  LZW_LOG(file_num, format, ##__VA_ARGS__)

#else
#define RECORD_INFO(file_num, format, ...)
#endif

#define PRI_DEBUG
#ifdef PRI_DEBUG
#define DBG_PRINT(format, a...) \
  printf(" DEBUG:%4d %-20s : " format, __LINE__, __FUNCTION__, ##a)
#else
#define DBG_PRINT(format, a...)
#endif

#define LZW_DEBUG

#ifdef LZW_DEBUG
#define RECORD_LOG(format, ...) LZW_LOG(0, format, ##__VA_ARGS__)

#else
#define RECORD_LOG(format, ...)
#endif

const std::string log_file0("./exp_log");
const std::string log_file1("./exp_op_time");
const std::string log_file2("./exp_op_data");
const std::string log_file3("./exp_stall.csv");
const std::string log_file4("./exp_compaction.csv");
const std::string log_file5("./exp_flush.csv");
const std::string log_file6("./exp_split.csv");
const std::string log_file7("./exp_bucket_general");
const std::string log_file8("./exp_bucket_detail");
const std::string log_file9("./exp_thread_pool.csv");
// const std::string log_file6("./exp_compaction_score.csv");
// const std::string log_file8("./exp_background_schedule.csv");
// const std::string log_file9("./exp_compaction_noting_to_do.csv");
// const std::string log_file10("./exp_switch_memtable.csv");

extern void init_log_file();

extern void LZW_LOG(int file_num, const char* format, ...);
extern uint64_t bench_start_time;