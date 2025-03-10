#pragma once

/*!
 * @file runtime.h
 * Setup and launcher for the runtime.
 */

#include "common/common_types.h"
#include <thread>

extern u8* g_ee_main_mem;
u32 exec_runtime(int argc, char** argv);

extern std::thread::id g_main_thread_id;
