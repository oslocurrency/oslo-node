#pragma once

#include <oslo/lib/work.hpp>
#include <oslo/node/openclconfig.hpp>
#include <oslo/node/xorshift.hpp>

#include <boost/optional.hpp>

#include <atomic>
#include <mutex>
#include <vector>

#ifdef __APPLE__
#define CL_SILENCE_DEPRECATION
#include <OpenCL/opencl.h>
#else
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS
#include <CL/cl.h>
#endif

namespace oslo
{
class logger_mt;
class opencl_platform
{
public:
	cl_platform_id platform;
	std::vector<cl_device_id> devices;
};
class opencl_environment
{
public:
	opencl_environment (bool &);
	void dump (std::ostream & stream);
	std::vector<oslo::opencl_platform> platforms;
};
class root;
class work_pool;
class opencl_work
{
public:
	opencl_work (bool &, oslo::opencl_config const &, oslo::opencl_environment &, oslo::logger_mt &);
	~opencl_work ();
	boost::optional<uint64_t> generate_work (oslo::work_version const, oslo::root const &, uint64_t const);
	boost::optional<uint64_t> generate_work (oslo::work_version const, oslo::root const &, uint64_t const, std::atomic<int> &);
	static std::unique_ptr<opencl_work> create (bool, oslo::opencl_config const &, oslo::logger_mt &);
	oslo::opencl_config const & config;
	std::mutex mutex;
	cl_context context;
	cl_mem attempt_buffer;
	cl_mem result_buffer;
	cl_mem item_buffer;
	cl_mem difficulty_buffer;
	cl_program program;
	cl_kernel kernel;
	cl_command_queue queue;
	oslo::xorshift1024star rand;
	oslo::logger_mt & logger;
};
}
