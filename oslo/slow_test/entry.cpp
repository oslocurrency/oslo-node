#include <oslo/node/common.hpp>

#include <gtest/gtest.h>
namespace oslo
{
void cleanup_test_directories_on_exit ();
void force_oslo_test_network ();
}

int main (int argc, char ** argv)
{
	oslo::force_oslo_test_network ();
	oslo::node_singleton_memory_pool_purge_guard memory_pool_cleanup_guard;
	testing::InitGoogleTest (&argc, argv);
	auto res = RUN_ALL_TESTS ();
	oslo::cleanup_test_directories_on_exit ();
	return res;
}
