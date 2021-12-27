#include "gtest/gtest.h"

#include <oslo/node/common.hpp>
#include <oslo/node/logging.hpp>

#include <boost/filesystem/path.hpp>

namespace oslo
{
void cleanup_test_directories_on_exit ();
void force_oslo_test_network ();
boost::filesystem::path unique_path ();
}

GTEST_API_ int main (int argc, char ** argv)
{
	printf ("Running main() from core_test_main.cc\n");
	oslo::force_oslo_test_network ();
	oslo::node_singleton_memory_pool_purge_guard memory_pool_cleanup_guard;
	// Setting up logging so that there aren't any piped to standard output.
	oslo::logging logging;
	logging.init (oslo::unique_path ());
	testing::InitGoogleTest (&argc, argv);
	auto res = RUN_ALL_TESTS ();
	oslo::cleanup_test_directories_on_exit ();
	return res;
}
