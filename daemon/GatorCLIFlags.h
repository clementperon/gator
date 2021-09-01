/* Copyright (C) 2010-2021 by Arm Limited. All rights reserved. */

#ifndef GATORCLIFLAGS_H_
#define GATORCLIFLAGS_H_

enum {
    USE_CMDLINE_ARG_SAMPLE_RATE = 0x01,
    USE_CMDLINE_ARG_CAPTURE_WORKING_DIR = 0x02,
    USE_CMDLINE_ARG_CAPTURE_COMMAND = 0x04,
    USE_CMDLINE_ARG_STOP_GATOR = 0x08,
    USE_CMDLINE_ARG_CALL_STACK_UNWINDING = 0x10,
    USE_CMDLINE_ARG_DURATION = 0x20,
    USE_CMDLINE_ARG_FTRACE_RAW = 0x40,
    USE_CMDLINE_ARG_EXCLUDE_KERNEL = 0x80,
};

#endif /* GATORCLIFLAGS_H_ */
