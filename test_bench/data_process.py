import re


def process_op_data(input_file):
    file = open(input_file)
    file_str = file.read()
    # print(file)
    file.close()
    # pattern = r'now=\s+(\d+\.\d+)\s+s\n([\s\S]*?)(?=now=|$)'
    pattern = r'now=\s+(\d+\.\d+)\s+s\s+'\
        '\*\* Compaction Stats \[default\] \*\*\n([\s\S]*?)'\
        '\*\* Compaction Stats \[default\] \*\*\n([\s\S]*?)'\
        '(Blob file count:[\s\S]+?)'\
        '\*\* File Read Latency Histogram By Level \[default\] \*\*\n([\s\S]*?)'\
        '\*\* DB Stats \*\*\n([\s\S]*?)'\
        '(?=now=|$)'
    matches = re.findall(pattern, file_str)
    time = []
    write_throughput = []
    avg_write_throughput = []
    interval_compaction_throughput=[]
    for match in matches:
        time.append(float(match[0]))
        #!db_stats
        # db_stats=match[5]
        # db_stats_pattern = r"\d+\:\d+\:\d+\.\d+|\d+\:\d+\.\d+|\d+\.\d+|\d+"
        # db_stats_match = re.findall(db_stats_pattern, db_stats)
        # avg_write_throughput.append(float(db_stats_match[7]))
        # write_throughput.append(float(db_stats_match[20]))
        #!compaction_stats
        compaction_stats = match[3]
        compaction_stats_pattern = r"\d+\:\d+\:\d+\.\d+|\d+\:\d+\.\d+|\d+\.\d+|\d+"
        compaction_stats_match = re.findall(
            compaction_stats_pattern, compaction_stats)
        # print(compaction_stats_match[21])
        interval_compaction_throughput.append(float(compaction_stats_match[21]))
    # print(write_throughput)
    return time, interval_compaction_throughput

# def get_name_list(input_file):
# 	name_list = []

# 	# 载入并编译正则表达式
# 	pattern = re.compile(r'公司$')

# 	with open(input_file, 'r', encoding='utf-8') as fr:
# 		for line in fr:
# 			# 匹配每行结尾是“公司”的行
# 			match = pattern.search(line)
# 			if match:
# 				name_list.append(line.strip('\n'))
# 	print(name_list)
# 	return name_list


# process_op_data("test")
