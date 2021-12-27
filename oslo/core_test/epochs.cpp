#include <oslo/lib/epoch.hpp>
#include <oslo/secure/common.hpp>

#include <gtest/gtest.h>

TEST (epochs, is_epoch_link)
{
	oslo::epochs epochs;
	// Test epoch 1
	oslo::keypair key1;
	auto link1 = 42;
	auto link2 = 43;
	ASSERT_FALSE (epochs.is_epoch_link (link1));
	ASSERT_FALSE (epochs.is_epoch_link (link2));
	epochs.add (oslo::epoch::epoch_1, key1.pub, link1);
	ASSERT_TRUE (epochs.is_epoch_link (link1));
	ASSERT_FALSE (epochs.is_epoch_link (link2));
	ASSERT_EQ (key1.pub, epochs.signer (oslo::epoch::epoch_1));
	ASSERT_EQ (epochs.epoch (link1), oslo::epoch::epoch_1);

	// Test epoch 2
	oslo::keypair key2;
	epochs.add (oslo::epoch::epoch_2, key2.pub, link2);
	ASSERT_TRUE (epochs.is_epoch_link (link2));
	ASSERT_EQ (key2.pub, epochs.signer (oslo::epoch::epoch_2));
	ASSERT_EQ (oslo::uint256_union (link1), epochs.link (oslo::epoch::epoch_1));
	ASSERT_EQ (oslo::uint256_union (link2), epochs.link (oslo::epoch::epoch_2));
	ASSERT_EQ (epochs.epoch (link2), oslo::epoch::epoch_2);
}

TEST (epochs, is_sequential)
{
	ASSERT_TRUE (oslo::epochs::is_sequential (oslo::epoch::epoch_0, oslo::epoch::epoch_1));
	ASSERT_TRUE (oslo::epochs::is_sequential (oslo::epoch::epoch_1, oslo::epoch::epoch_2));

	ASSERT_FALSE (oslo::epochs::is_sequential (oslo::epoch::epoch_0, oslo::epoch::epoch_2));
	ASSERT_FALSE (oslo::epochs::is_sequential (oslo::epoch::epoch_0, oslo::epoch::invalid));
	ASSERT_FALSE (oslo::epochs::is_sequential (oslo::epoch::unspecified, oslo::epoch::epoch_1));
	ASSERT_FALSE (oslo::epochs::is_sequential (oslo::epoch::epoch_1, oslo::epoch::epoch_0));
	ASSERT_FALSE (oslo::epochs::is_sequential (oslo::epoch::epoch_2, oslo::epoch::epoch_0));
	ASSERT_FALSE (oslo::epochs::is_sequential (oslo::epoch::epoch_2, oslo::epoch::epoch_2));
}
