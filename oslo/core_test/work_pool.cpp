#include <oslo/crypto_lib/random_pool.hpp>
#include <oslo/lib/blocks.hpp>
#include <oslo/lib/jsonconfig.hpp>
#include <oslo/lib/logger_mt.hpp>
#include <oslo/lib/timer.hpp>
#include <oslo/lib/work.hpp>
#include <oslo/node/logging.hpp>
#include <oslo/node/openclconfig.hpp>
#include <oslo/node/openclwork.hpp>
#include <oslo/secure/common.hpp>
#include <oslo/secure/utility.hpp>

#include <gtest/gtest.h>

#include <future>

TEST (work, one)
{
	oslo::network_constants network_constants;
	oslo::work_pool pool (std::numeric_limits<unsigned>::max ());
	oslo::change_block block (1, 1, oslo::keypair ().prv, 3, 4);
	block.block_work_set (*pool.generate (block.root ()));
	ASSERT_LT (oslo::work_threshold_base (block.work_version ()), block.difficulty ());
}

TEST (work, disabled)
{
	oslo::network_constants network_constants;
	oslo::work_pool pool (0);
	auto result (pool.generate (oslo::block_hash ()));
	ASSERT_FALSE (result.is_initialized ());
}

TEST (work, validate)
{
	oslo::network_constants network_constants;
	oslo::work_pool pool (std::numeric_limits<unsigned>::max ());
	oslo::send_block send_block (1, 1, 2, oslo::keypair ().prv, 4, 6);
	ASSERT_LT (send_block.difficulty (), oslo::work_threshold_base (send_block.work_version ()));
	send_block.block_work_set (*pool.generate (send_block.root ()));
	ASSERT_LT (oslo::work_threshold_base (send_block.work_version ()), send_block.difficulty ());
}

TEST (work, cancel)
{
	oslo::work_pool pool (std::numeric_limits<unsigned>::max ());
	auto iterations (0);
	auto done (false);
	while (!done)
	{
		oslo::root key (1);
		pool.generate (
		oslo::work_version::work_1, key, oslo::network_constants ().publish_thresholds.base, [&done](boost::optional<uint64_t> work_a) {
			done = !work_a;
		});
		pool.cancel (key);
		++iterations;
		ASSERT_LT (iterations, 200);
	}
}

TEST (work, cancel_many)
{
	oslo::work_pool pool (std::numeric_limits<unsigned>::max ());
	oslo::root key1 (1);
	oslo::root key2 (2);
	oslo::root key3 (1);
	oslo::root key4 (1);
	oslo::root key5 (3);
	oslo::root key6 (1);
	oslo::network_constants constants;
	pool.generate (oslo::work_version::work_1, key1, constants.publish_thresholds.base, [](boost::optional<uint64_t>) {});
	pool.generate (oslo::work_version::work_1, key2, constants.publish_thresholds.base, [](boost::optional<uint64_t>) {});
	pool.generate (oslo::work_version::work_1, key3, constants.publish_thresholds.base, [](boost::optional<uint64_t>) {});
	pool.generate (oslo::work_version::work_1, key4, constants.publish_thresholds.base, [](boost::optional<uint64_t>) {});
	pool.generate (oslo::work_version::work_1, key5, constants.publish_thresholds.base, [](boost::optional<uint64_t>) {});
	pool.generate (oslo::work_version::work_1, key6, constants.publish_thresholds.base, [](boost::optional<uint64_t>) {});
	pool.cancel (key1);
}

TEST (work, opencl)
{
	oslo::logging logging;
	logging.init (oslo::unique_path ());
	oslo::logger_mt logger;
	bool error (false);
	oslo::opencl_environment environment (error);
	ASSERT_FALSE (error);
	if (!environment.platforms.empty () && !environment.platforms.begin ()->devices.empty ())
	{
		oslo::opencl_config config (0, 0, 16 * 1024);
		auto opencl (oslo::opencl_work::create (true, config, logger));
		if (opencl != nullptr)
		{
			// 0 threads, should add 1 for managing OpenCL
			oslo::work_pool pool (0, std::chrono::nanoseconds (0), [&opencl](oslo::work_version const version_a, oslo::root const & root_a, uint64_t difficulty_a, std::atomic<int> & ticket_a) {
				return opencl->generate_work (version_a, root_a, difficulty_a);
			});
			ASSERT_NE (nullptr, pool.opencl);
			oslo::root root;
			uint64_t difficulty (0xff00000000000000);
			uint64_t difficulty_add (0x000f000000000000);
			for (auto i (0); i < 16; ++i)
			{
				oslo::random_pool::generate_block (root.bytes.data (), root.bytes.size ());
				auto result (*pool.generate (oslo::work_version::work_1, root, difficulty));
				ASSERT_GE (oslo::work_difficulty (oslo::work_version::work_1, root, result), difficulty);
				difficulty += difficulty_add;
			}
		}
		else
		{
			std::cerr << "Error starting OpenCL test" << std::endl;
		}
	}
	else
	{
		std::cout << "Device with OpenCL support not found. Skipping OpenCL test" << std::endl;
	}
}

TEST (work, opencl_config)
{
	oslo::opencl_config config1;
	config1.platform = 1;
	config1.device = 2;
	config1.threads = 3;
	oslo::jsonconfig tree;
	config1.serialize_json (tree);
	oslo::opencl_config config2;
	ASSERT_FALSE (config2.deserialize_json (tree));
	ASSERT_EQ (1, config2.platform);
	ASSERT_EQ (2, config2.device);
	ASSERT_EQ (3, config2.threads);
}

TEST (work, difficulty)
{
	oslo::work_pool pool (std::numeric_limits<unsigned>::max ());
	oslo::root root (1);
	uint64_t difficulty1 (0xff00000000000000);
	uint64_t difficulty2 (0xfff0000000000000);
	uint64_t difficulty3 (0xffff000000000000);
	uint64_t result_difficulty1 (0);
	do
	{
		auto work1 = *pool.generate (oslo::work_version::work_1, root, difficulty1);
		result_difficulty1 = oslo::work_difficulty (oslo::work_version::work_1, root, work1);
	} while (result_difficulty1 > difficulty2);
	ASSERT_GT (result_difficulty1, difficulty1);
	uint64_t result_difficulty2 (0);
	do
	{
		auto work2 = *pool.generate (oslo::work_version::work_1, root, difficulty2);
		result_difficulty2 = oslo::work_difficulty (oslo::work_version::work_1, root, work2);
	} while (result_difficulty2 > difficulty3);
	ASSERT_GT (result_difficulty2, difficulty2);
}

TEST (work, eco_pow)
{
	auto work_func = [](std::promise<std::chrono::nanoseconds> & promise, std::chrono::nanoseconds interval) {
		oslo::work_pool pool (1, interval);
		constexpr auto num_iterations = 5;

		oslo::timer<std::chrono::nanoseconds> timer;
		timer.start ();
		for (int i = 0; i < num_iterations; ++i)
		{
			oslo::root root (1);
			uint64_t difficulty1 (0xff00000000000000);
			uint64_t difficulty2 (0xfff0000000000000);
			uint64_t result_difficulty (0);
			do
			{
				auto work = *pool.generate (oslo::work_version::work_1, root, difficulty1);
				result_difficulty = oslo::work_difficulty (oslo::work_version::work_1, root, work);
			} while (result_difficulty > difficulty2);
			ASSERT_GT (result_difficulty, difficulty1);
		}

		promise.set_value_at_thread_exit (timer.stop ());
	};

	std::promise<std::chrono::nanoseconds> promise1;
	std::future<std::chrono::nanoseconds> future1 = promise1.get_future ();
	std::promise<std::chrono::nanoseconds> promise2;
	std::future<std::chrono::nanoseconds> future2 = promise2.get_future ();

	std::thread thread1 (work_func, std::ref (promise1), std::chrono::nanoseconds (0));
	std::thread thread2 (work_func, std::ref (promise2), std::chrono::milliseconds (10));

	thread1.join ();
	thread2.join ();

	// Confirm that the eco pow rate limiter is working.
	// It's possible under some unlucky circumstances that this fails to the random nature of valid work generation.
	ASSERT_LT (future1.get (), future2.get ());
}
