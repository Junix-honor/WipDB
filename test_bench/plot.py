import datetime
import time
import glob
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from matplotlib.pyplot import MultipleLocator
# from data_process import *
from collections import Counter

NAME = "1683357583_wipdb_result_80000000_64M_1user_3high_3low"

fig, (throughput_ax, cpu_ax, high_cpu_ax, low_cpu_ax, pidstat_io_write_ax,  pidstat_high_io_write_ax,  pidstat_low_io_write_ax, pidstat_io_read_ax, pidstat_high_io_read_ax, pidstat_low_io_read_ax, io_bw_ax, io_req_size_ax, io_que_size_ax) = plt.subplots(13, 1, figsize=(
50, 50), layout='constrained', sharex=True)
fig.suptitle(NAME, fontsize="xx-large", fontweight="medium")

#! start_time
with open('./data/'+NAME+'/exp_op_time', 'r') as f:
    number = int(f.read())
# print(number)
db_start_time_raw = time.localtime(number)
# print(db_start_time_raw)
db_start_time_str = str(db_start_time_raw[3])+":" + \
    str(db_start_time_raw[4])+":"+str(db_start_time_raw[5])
# print(db_start_time_str)
db_start_time = time.strptime(db_start_time_str, "%H:%M:%S")
# print(db_start_time)


#! throughput
files = glob.glob('./data/'+NAME+'/exp_throughput_*')
max_time = 0
first = True
# print(files)
for file in files:
    throughput_data = pd.read_csv(file, encoding='utf-8',
                                  names=["now", "bw", "iops", "size", "average bw", "average iops"], header=0)
    max_time = max(max_time, throughput_data["now"].iloc[-1])
    throughput_ax.plot(throughput_data["now"],
                       throughput_data["bw"], label=file.split('/')[-1], alpha=0.7)
    throughput_ax.plot(throughput_data["now"], throughput_data["average bw"],
                       "-.", label=file.split('/')[-1]+" average", alpha=0.7)
    if first:
        throughput_ax.text(throughput_data["now"].iloc[-1], throughput_data["average bw"].iloc[-1]+0.1,
                           '%.0f' % throughput_data["average bw"].iloc[-1], ha='center', va='bottom', fontsize='small', rotation=0)
        first = False

x_major_locator = MultipleLocator(10)
throughput_ax.set_xlim(xmin=0, xmax=max_time)
throughput_ax.xaxis.set_major_locator(x_major_locator)
throughput_ax.set_xlabel("Time(s)")
throughput_ax.set_ylabel("Write throughput(MB/s)")
throughput_ax.grid()
throughput_ax.set_title("foreground write throughput, stall and delay situation", fontsize="large",
                        loc='left', fontstyle='oblique')

#! top
top_time_data = pd.read_csv('./data/'+NAME+'/exp_top_time.txt', encoding='utf-8',
                            delim_whitespace=True, names=["TIME"])
time_list = top_time_data["TIME"].tolist()
relative_time_list = []
# time_start = time.strptime(time_list[0],"%H:%M:%S")
for i in range(0, len(time_list)):
    time_now = time.strptime(time_list[i], "%H:%M:%S")
    relative_time = int(time.mktime(time_now))-int(time.mktime(db_start_time))
    if relative_time<0:
        relative_time+=86400
    relative_time_list.append(relative_time)

# print(relative_time_list)

top_data = pd.read_csv('./data/'+NAME+'/exp_top.txt', encoding='utf-8',
                       delim_whitespace=True, names=["PID", "USER", "PR", "NI", "VIRT", "RES", "SHR", "S", "CPU", "MEM", "TIME", "COMMAND"])
high_cpu_lists = []
low_cpu_lists = []
top_min_len = len(relative_time_list)
top_data_grouped = top_data.groupby(["PID"])


def process_top_data_group(group):
    cpu_list = group["CPU"].tolist()
    cpu_list_len = len(cpu_list)
    global top_min_len
    top_min_len = min(top_min_len, cpu_list_len)
    thread_name = group["COMMAND"].tolist()[0]
    if "bench" in thread_name:
        cpu_ax.plot(relative_time_list[:cpu_list_len], cpu_list,
                    label=thread_name+"_"+str(group.name), alpha=0.7)
    elif "high" in thread_name:
        high_cpu_ax.plot(relative_time_list[:cpu_list_len], cpu_list,
                         label=thread_name+"_"+str(group.name), alpha=0.7)
        high_cpu_lists.append(cpu_list)
    elif "low" in thread_name:
        low_cpu_ax.plot(relative_time_list[:cpu_list_len], cpu_list,
                        label=thread_name+"_"+str(group.name), alpha=0.7)
        low_cpu_lists.append(cpu_list)


top_data_grouped.apply(process_top_data_group)

high_cpu_list = []
low_cpu_list = []
for i in range(top_min_len):
    total = 0
    for list in high_cpu_lists:
        total += list[i]
    if len(high_cpu_lists) != 0:
        high_cpu_list.append(total/len(high_cpu_lists))
    else:
        high_cpu_list.append(0)
for i in range(top_min_len):
    total = 0
    for list in low_cpu_lists:
        total += list[i]
    if len(low_cpu_lists) != 0:
        low_cpu_list.append(total/len(low_cpu_lists))
    else:
        low_cpu_list.append(0)
cpu_ax.plot(relative_time_list[:top_min_len], high_cpu_list,
            label="high_thread_pool_avg", alpha=0.7)
cpu_ax.plot(relative_time_list[:top_min_len], low_cpu_list,
            label="low_thread_pool_avg", alpha=0.7)

avg_high_cpu_list = []
avg_low_cpu_list = []
for i in range(0, top_min_len):
    avg_high_cpu_list.append(np.mean(high_cpu_list[:i+1]))
for i in range(0, top_min_len):
    avg_low_cpu_list.append(np.mean(low_cpu_list[:i+1]))
cpu_ax.plot(relative_time_list[:top_min_len], avg_high_cpu_list, "-.",
            label="high_thread_pool_avg_avg", alpha=0.7)
cpu_ax.text(relative_time_list[top_min_len-1], avg_high_cpu_list[-1]+0.1,
            '%.0f' % avg_high_cpu_list[-1], ha='center', va='bottom', fontsize='small', rotation=0)
cpu_ax.plot(relative_time_list[:top_min_len], avg_low_cpu_list, "-.",
            label="low_thread_pool_avg_avg", alpha=0.7)
cpu_ax.text(relative_time_list[top_min_len-1], avg_low_cpu_list[-1]+0.1,
            '%.0f' % avg_low_cpu_list[-1], ha='center', va='bottom', fontsize='small', rotation=0)

cpu_ax.set_xlabel("Time(s)")
cpu_ax.set_ylabel("CPU Utilization(%)")
cpu_ax.grid()
cpu_ax.set_title("foreground db_bench CPU utilization, background high and low thread pool average CPU utilization", fontsize="large",
                 loc='left', fontstyle='oblique')

high_cpu_ax.set_xlabel("Time(s)")
high_cpu_ax.set_ylabel("CPU Utilization(%)")
high_cpu_ax.grid()
high_cpu_ax.set_title("background high thread pool CPU utilization", fontsize="large",
                      loc='left', fontstyle='oblique')

low_cpu_ax.set_xlabel("Time(s)")
low_cpu_ax.set_ylabel("CPU Utilization(%)")
low_cpu_ax.grid()
low_cpu_ax.set_title("background low thread pool CPU utilization", fontsize="large",
                     loc='left', fontstyle='oblique')


#! io_stat
io_time_data = pd.read_csv('./data/'+NAME+'/exp_iostat_time.txt', encoding='utf-8',
                           delim_whitespace=True, names=["TIME"])
time_list = io_time_data["TIME"].tolist()
relative_time_list = []
# time_start = time.strptime(time_list[0], "%H:%M:%S")
for i in range(0, len(time_list)):
    time_now = time.strptime(time_list[i], "%H:%M:%S")
    relative_time = int(time.mktime(time_now))-int(time.mktime(db_start_time))
    if relative_time < 0:
        relative_time += 86400
    relative_time_list.append(relative_time)

io_data = pd.read_csv('./data/'+NAME+'/exp_iostat.txt', encoding='utf-8',
                      delim_whitespace=True, names=["Device","rrqm/s","wrqm/s","r/s","w/s","rMB/s","wMB/s","avgrq-sz","avgqu-sz","await","r_await","w_await","svctm","%util"])

io_data.loc[:, 'wareq-sz'] = io_data.loc[:, 'wareq-sz']*512/1024
io_data_grouped = io_data.groupby(["Device"])


def process_io_data_group(group):
    # write_bandwith = group["wMB/s"].tolist()
    # read_bandwith = group["rMB/s"].tolist()
    io_bw_ax.plot(relative_time_list, group["wMB/s"],
                  label="write_"+group["Device"].tolist()[0], alpha=0.7)
    io_bw_ax.text(relative_time_list[-1], group["wMB/s"].mean()+10,
                  '%.2f' % group["wMB/s"].mean(), ha='center', va='bottom', fontsize='small', rotation=0)
    io_bw_ax.plot(relative_time_list, group["rMB/s"],
                  label="read_"+group["Device"].tolist()[0], alpha=0.7)
    io_bw_ax.text(relative_time_list[-1], group["rMB/s"].mean()-10,
                  '%.2f' % group["rMB/s"].mean(), ha='center', va='bottom', fontsize='small', rotation=0)
    sum_bw = group["wMB/s"]+group["rMB/s"]
    io_bw_ax.plot(relative_time_list, sum_bw,
                  label=group["Device"].tolist()[0]+"_total_bandwith", alpha=0.7)
    io_bw_ax.text(relative_time_list[-1]-40, sum_bw.mean()-1,
                  '%.2f' % sum_bw.mean(), ha='center', va='bottom', fontsize='small', rotation=0)
    # avg_write_bandwith = []
    # for i in range(0, len(write_bandwith)):
    #     avg_write_bandwith.append(np.mean(write_bandwith[:i+1]))
    # io_bw_ax.plot(relative_time_list, avg_write_bandwith, "-.",
    #               label=group["Device"].tolist()[0]+"_avg", alpha=0.7)
    # io_bw_ax.text(relative_time_list[-1], avg_write_bandwith[-1]+0.1,
    #               '%.2f' % avg_write_bandwith[-1], ha='center', va='bottom', fontsize='small', rotation=0)
    io_req_size_ax.plot(relative_time_list, group["avgrq-sz"],
                        label=+group["Device"].tolist()[0], alpha=0.7)
    io_req_size_ax.text(relative_time_list[-1], group["avgrq-sz"].mean()+20,
                        '%.2f' % group["avgrq-sz"].mean(), ha='center', va='bottom', fontsize='small', rotation=0)
    
    # io_req_size_ax.plot(relative_time_list, group["rareq-sz"],
    #                     label="read_"+group["Device"].tolist()[0], alpha=0.7)
    # io_req_size_ax.text(relative_time_list[-1], group["rareq-sz"].mean()-20,
    #                     '%.2f' % group["rareq-sz"].mean(), ha='center', va='bottom', fontsize='small', rotation=0)
    
    io_que_size_ax.plot(relative_time_list, group["avgqu-sz"],
                        label=group["Device"].tolist()[0], alpha=0.7)
    io_que_size_ax.text(relative_time_list[-1]-1, group["avgqu-sz"].mean()+0.1,
                        '%.2f' % group["avgqu-sz"].mean(), ha='center', va='bottom', fontsize='small', rotation=0)
    # aqu_sz = group["avgqu-sz"].mean()
    # print(group["Device"].tolist()[0])
    # print(aqu_sz)


io_data_grouped.apply(process_io_data_group)

io_bw_ax.set_xlabel("Time(s)")
io_bw_ax.set_ylabel("IO Bandwith(MB/s)")
io_bw_ax.grid()
io_bw_ax.set_title("device IO bandwith", fontsize="large",
                   loc='left', fontstyle='oblique')

io_req_size_ax.set_xlabel("Time(s)")
io_req_size_ax.set_ylabel("Write Avg Req Size(MB)")
io_req_size_ax.grid()
io_req_size_ax.set_title("write average request size", fontsize="large",
                         loc='left', fontstyle='oblique')

io_que_size_ax.set_xlabel("Time(s)")
io_que_size_ax.set_ylabel("Number")
io_que_size_ax.grid()
io_que_size_ax.set_title("averge queue size", fontsize="large",
                   loc='left', fontstyle='oblique')

#! pidstat
pidstat_data = pd.read_csv('./data/'+NAME+'/exp_pidstat_io.txt', encoding='utf-8',
                           delim_whitespace=True, names=["TIME", "UID", "TGID", "TID", "kB_rd/s", "kB_wr/s", "kB_ccwr/s", "iodelay", "COMMAND"])
pidstat_data.loc[:, 'kB_wr/s'] = pidstat_data.loc[:, 'kB_wr/s']/1024.0
pidstat_data.loc[:, 'kB_rd/s'] = pidstat_data.loc[:, 'kB_rd/s']/1024.0


time_list = pidstat_data['TIME'].drop_duplicates().tolist()
relative_time_list = []
for i in range(0, len(time_list)):
    time_now = time.strptime(time_list[i], "%H:%M:%S")
    relative_time = int(time.mktime(time_now)) - \
        int(time.mktime(db_start_time))
    if relative_time < 0:
        relative_time += 86400
    relative_time_list.append(relative_time)
pidstat_min_len = len(relative_time_list)
high_io_write_lists = []
low_io_write_lists = []
high_io_read_lists = []
low_io_read_lists = []

pidstat_data_grouped = pidstat_data.groupby(["TID"])


def process_pidstat_data_group(group):
    wr_len = len(group["kB_wr/s"])
    global pidstat_min_len
    pidstat_min_len = min(pidstat_min_len, wr_len)
    thread_name = group["COMMAND"].tolist()[0][3:]
    if "bench" in thread_name:
        pidstat_io_write_ax.plot(relative_time_list[:wr_len], group["kB_wr/s"], label=thread_name +
                           "_"+str(group.name), alpha=0.7)
        pidstat_io_read_ax.plot(relative_time_list[:wr_len], group["kB_rd/s"], label=thread_name +
                                 "_"+str(group.name), alpha=0.7)
        # bench_lists.append(wr)
    elif "high" in thread_name:
        pidstat_high_io_write_ax.plot(relative_time_list[:wr_len], group["kB_wr/s"], label=thread_name +
                                "_"+str(group.name), alpha=0.7)
        high_io_write_lists.append(group["kB_wr/s"].tolist())
        pidstat_high_io_read_ax.plot(relative_time_list[:wr_len], group["kB_rd/s"], label=thread_name +
                                      "_"+str(group.name), alpha=0.7)
        high_io_read_lists.append(group["kB_rd/s"].tolist())
    elif "low" in thread_name:
        pidstat_low_io_write_ax.plot(relative_time_list[:wr_len], group["kB_wr/s"], label=thread_name +
                               "_"+str(group.name), alpha=0.7)
        low_io_write_lists.append(group["kB_wr/s"].tolist())
        pidstat_low_io_read_ax.plot(relative_time_list[:wr_len], group["kB_rd/s"], label=thread_name +
                                     "_"+str(group.name), alpha=0.7)
        low_io_read_lists.append(group["kB_rd/s"].tolist())


pidstat_data_grouped.apply(process_pidstat_data_group)

high_io_write_list = []
low_io_write_list = []
for i in range(pidstat_min_len):
    total = 0
    for list in high_io_write_lists:
        total += list[i]
    high_io_write_list.append(total)
for i in range(pidstat_min_len):
    total = 0
    for list in low_io_write_lists:
        total += list[i]
    low_io_write_list.append(total)
pidstat_io_write_ax.plot(relative_time_list[:pidstat_min_len],
                   high_io_write_list, label="high_thread_pool_total", alpha=0.7)
pidstat_io_write_ax.plot(relative_time_list[:pidstat_min_len],
                   low_io_write_list, label="low_thread_pool_total", alpha=0.7)


high_io_read_list = []
low_io_read_list = []
for i in range(pidstat_min_len):
    total = 0
    for list in high_io_read_lists:
        total += list[i]
    high_io_read_list.append(total)
for i in range(pidstat_min_len):
    total = 0
    for list in low_io_read_lists:
        total += list[i]
    low_io_read_list.append(total)
pidstat_io_read_ax.plot(relative_time_list[:pidstat_min_len],
                         high_io_read_list, label="high_thread_pool_total", alpha=0.7)
pidstat_io_read_ax.plot(relative_time_list[:pidstat_min_len],
                         low_io_read_list, label="low_thread_pool_total", alpha=0.7)


avg_high_io_write_list = []
avg_low_io_write_list = []
for i in range(0, pidstat_min_len):
    avg_high_io_write_list.append(np.mean(high_io_write_list[:i+1]))
for i in range(0, pidstat_min_len):
    avg_low_io_write_list.append(np.mean(low_io_write_list[:i+1]))
pidstat_io_write_ax.plot(relative_time_list[:pidstat_min_len], avg_high_io_write_list, "-.",
                   label="high_thread_pool_total_avg", alpha=0.7)
pidstat_io_write_ax.text(relative_time_list[pidstat_min_len-1], avg_high_io_write_list[-1]+0.1,
                   '%.0f' % avg_high_io_write_list[-1], ha='center', va='bottom', fontsize='small', rotation=0)
pidstat_io_write_ax.plot(relative_time_list[:pidstat_min_len], avg_low_io_write_list, "-.",
                   label="low_thread_pool_total_avg", alpha=0.7)
pidstat_io_write_ax.text(relative_time_list[pidstat_min_len-1], avg_low_io_write_list[-1]+0.1,
                   '%.0f' % avg_low_io_write_list[-1], ha='center', va='bottom', fontsize='small', rotation=0)

avg_high_io_read_list = []
avg_low_io_read_list = []
for i in range(0, pidstat_min_len):
    avg_high_io_read_list.append(np.mean(high_io_read_list[:i+1]))
for i in range(0, pidstat_min_len):
    avg_low_io_read_list.append(np.mean(low_io_read_list[:i+1]))
pidstat_io_read_ax.plot(relative_time_list[:pidstat_min_len], avg_high_io_read_list, "-.",
                         label="high_thread_pool_total_avg", alpha=0.7)
pidstat_io_read_ax.text(relative_time_list[pidstat_min_len-1], avg_high_io_read_list[-1]+0.1,
                         '%.0f' % avg_high_io_read_list[-1], ha='center', va='bottom', fontsize='small', rotation=0)
pidstat_io_read_ax.plot(relative_time_list[:pidstat_min_len], avg_low_io_read_list, "-.",
                         label="low_thread_pool_total_avg", alpha=0.7)
pidstat_io_read_ax.text(relative_time_list[pidstat_min_len-1], avg_low_io_read_list[-1]+0.1,
                         '%.0f' % avg_low_io_read_list[-1], ha='center', va='bottom', fontsize='small', rotation=0)

# pidstat_data_grouped_time = pidstat_data.groupby(
#     ["TIME"]).agg({'kB_wr/s': 'sum'})
# io_bw_ax.plot(relative_time_list, pidstat_data_grouped_time["kB_wr/s"].tolist(),
#               label="sum", alpha=0.5)

pidstat_io_write_ax.set_xlabel("Time(s)")
pidstat_io_write_ax.set_ylabel("IO Bandwith(MB/s)")
pidstat_io_write_ax.grid()
pidstat_io_write_ax.set_title("foreground db_bench IO bandwith, background high and low thread pool total IO write bandwith", fontsize="large",
                        loc='left', fontstyle='oblique')

pidstat_high_io_write_ax.set_xlabel("Time(s)")
pidstat_high_io_write_ax.set_ylabel("IO Bandwith(MB/s)")
pidstat_high_io_write_ax.grid()
pidstat_high_io_write_ax.set_title("background high thread pool IO write bandwith", fontsize="large",
                             loc='left', fontstyle='oblique')

pidstat_low_io_write_ax.set_xlabel("Time(s)")
pidstat_low_io_write_ax.set_ylabel("IO Bandwith(MB/s)")
pidstat_low_io_write_ax.grid()
pidstat_low_io_write_ax.set_title("background low thread pool IO write bandwith", fontsize="large",
                            loc='left', fontstyle='oblique')

pidstat_io_read_ax.set_xlabel("Time(s)")
pidstat_io_read_ax.set_ylabel("IO Bandwith(MB/s)")
pidstat_io_read_ax.grid()
pidstat_io_read_ax.set_title("foreground db_bench IO bandwith, background high and low thread pool total IO read bandwith", fontsize="large",
                              loc='left', fontstyle='oblique')

pidstat_high_io_read_ax.set_xlabel("Time(s)")
pidstat_high_io_read_ax.set_ylabel("IO Bandwith(MB/s)")
pidstat_high_io_read_ax.grid()
pidstat_high_io_read_ax.set_title("background high thread pool IO read bandwith", fontsize="large",
                                   loc='left', fontstyle='oblique')

pidstat_low_io_read_ax.set_xlabel("Time(s)")
pidstat_low_io_read_ax.set_ylabel("IO Bandwith(MB/s)")
pidstat_low_io_read_ax.grid()
pidstat_low_io_read_ax.set_title("background low thread pool IO read bandwith", fontsize="large",
                                  loc='left', fontstyle='oblique')

throughput_ax.legend(loc='lower left', ncols=4)
cpu_ax.legend(loc='upper left', ncols=4)
high_cpu_ax.legend(loc='upper left', ncols=4)
low_cpu_ax.legend(loc='upper left', ncols=4)
io_bw_ax.legend(loc='lower left', ncols=5)
io_req_size_ax.legend(loc='upper left', ncols=4)
pidstat_io_write_ax.legend(loc='upper left', ncols=4)
pidstat_high_io_write_ax.legend(loc='upper left', ncols=4)
pidstat_low_io_write_ax.legend(loc='upper left', ncols=4)
pidstat_io_read_ax.legend(loc='upper left', ncols=4)
pidstat_high_io_read_ax.legend(loc='upper left', ncols=4)
pidstat_low_io_read_ax.legend(loc='upper left', ncols=4)
plt.savefig('./data/'+NAME+'/'+NAME+'.jpg')
# plt.show()
