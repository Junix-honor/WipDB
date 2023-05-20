#include "perf_log.h"

// std::mutex exp_compaction_lock;
// std::mutex exp_flush_lock;
uint64_t bench_start_time;
void init_log_file() {
  bench_start_time = 0;
  FILE* fp;
#ifdef LZW_INFO
  fp = fopen(log_file1.c_str(), "w");
  if (fp == nullptr) printf("log failed\n");
  fclose(fp);
  // RECORD_INFO(1, "unix_time,now,bw,iops,size,average bw,average iops\n");

  fp = fopen(log_file2.c_str(), "w");
  if (fp == nullptr) printf("log failed\n");
  fclose(fp);

  fp = fopen(log_file3.c_str(), "w");
  if (fp == nullptr) printf("log failed\n");
  fclose(fp);
  RECORD_INFO(3, "db,type,cause,start,end\n");

  fp = fopen(log_file4.c_str(), "w");
  if (fp == nullptr) printf("log failed\n");
  fclose(fp);
  RECORD_INFO(4, "db,start,end,level\n");


  fp = fopen(log_file5.c_str(), "w");
  if (fp == nullptr) printf("log failed\n");
  fclose(fp);
  RECORD_INFO(5, "db,start,end\n");

  fp = fopen(log_file6.c_str(), "w");
  if (fp == nullptr) printf("log failed\n");
  fclose(fp);
  RECORD_INFO(6, "db,start,end\n");

  fp = fopen(log_file7.c_str(), "w");
  if (fp == nullptr) printf("log failed\n");
  fclose(fp);

  fp = fopen(log_file8.c_str(), "w");
  if (fp == nullptr) printf("log failed\n");
  fclose(fp);

  fp = fopen(log_file9.c_str(), "w");
  if (fp == nullptr) printf("log failed\n");
  fclose(fp);
  RECORD_INFO(9, "time,priority,num\n");

//   fp = fopen(log_file10.c_str(), "w");
//   if (fp == nullptr) printf("log failed\n");
//   fclose(fp);
//   RECORD_INFO(10, "time\n");

#endif

#ifdef LZW_DEBUG
  fp = fopen(log_file0.c_str(), "w");
  if (fp == nullptr) printf("log failed\n");
  fclose(fp);

#endif
}

void LZW_LOG(int file_num, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  char buf[8192];
  vsprintf(buf, format, ap);
  va_end(ap);

  const std::string* log_file;
  switch (file_num) {
    case 0:
      log_file = &log_file0;
      break;
    case 1:
      log_file = &log_file1;
      break;
    case 2:
      log_file = &log_file2;
      break;
    case 3:
      log_file = &log_file3;
      break;
    case 4:
      log_file = &log_file4;
      break;
    case 5:
      log_file = &log_file5;
      break;
    case 6:
      log_file = &log_file6;
      break;
    case 7:
      log_file = &log_file7;
      break;
    case 8:
      log_file = &log_file8;
      break;
    case 9:
      log_file = &log_file9;
      break;
    // case 10:
    //   log_file = &log_file10;
    //   break;
    default:
      return;
  }

  FILE* fp = fopen(log_file->c_str(), "a");
  if (fp == nullptr) printf("log failed\n");
  fprintf(fp, "%s", buf);
  fclose(fp);
}
