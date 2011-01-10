#!/usr/bin/env python
# Copyright (C) 2010 Freescale Semiconductor, Inc.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
#  THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR
#  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
#  OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
#  NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
#  TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
#  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
#  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
#  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
#  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

import os
import sys
import subprocess
import time
import shutil
import glob
import common
import re
import imp
import pexpect
import signal

if len(sys.argv) != 3:
	print "Usage: %s <test_plan_file> <target>\n" % sys.argv[0]
	print "(valid targets: p4080ds, p3041ds, p5020ds, p4080ds_hw)\n"
	sys.exit(1)

test_plan = sys.argv[1]
target = sys.argv[2]
plan = file(test_plan,'r')


#create log dir
log_dir = common.LOG_PATH+"%s/" % target
if os.path.exists(log_dir):
	shutil.rmtree(log_dir)
os.makedirs(log_dir)




results = target+"_results.txt"
resultsp = file(results,'w')
resultsp.write("Test Name,Result,Passes,Fails,Timeouts\n")
resultsp.close()

#start timer
time_start = time.time()


testn = 0
for line in plan:
	#format line
	line = line.rstrip()
	test_list = line.split(',')
	if len(test_list) != 5:
		print "Error in test plan!"
		continue
	testn += 1
	print "\n===Test %d===\n" % testn
	print test_list[0],"-",test_list[1],":",test_list[4]
	if test_list[4] == 'enabled':
		cmd = "python run_test.py %s %s %s %s %s %s" % \
		(test_list[0],test_list[1], test_list[2], test_list[3], target, results)
		print cmd
		part_time_start = time.time()
		child = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
		child.wait()
		print "Test duration:",time.strftime("%H:%M:%S", time.gmtime(time.time()-part_time_start))
		time.sleep(2)
	else:
		resultsp = file(results,'a')
		#format testname
		subtest = ""
		if test_list[1] != "none":
			subtest = "-"+test_list[1] 
		resultsp.write("%s%s,NOT_EXECUTED,,,\n" % (test_list[0],subtest))
		resultsp.flush()
		#resultsp.close()

#stop timer
time_stop = time.time()
print "\n\n======================================"
print "Total duration:",time.strftime("%H:%M:%S", time.gmtime(time_stop-time_start))
