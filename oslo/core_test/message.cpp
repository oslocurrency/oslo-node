#include <oslo/node/common.hpp>
#include <oslo/node/network.hpp>
#include <oslo/secure/buffer.hpp>

#include <gtest/gtest.h>

#include <boost/variant/get.hpp>

TEST (message, keepalive_serialization)
{
	oslo::keepalive request1;
	std::vector<uint8_t> bytes;
	{
		oslo::vectorstream stream (bytes);
		request1.serialize (stream, false);
	}
	auto error (false);
	oslo::bufferstream stream (bytes.data (), bytes.size ());
	oslo::message_header header (error, stream);
	ASSERT_FALSE (error);
	oslo::keepalive request2 (error, stream, header);
	ASSERT_FALSE (error);
	ASSERT_EQ (request1, request2);
}

TEST (message, keepalive_deserialize)
{
	oslo::keepalive message1;
	message1.peers[0] = oslo::endpoint (boost::asio::ip::address_v6::loopback (), 10000);
	std::vector<uint8_t> bytes;
	{
		oslo::vectorstream stream (bytes);
		message1.serialize (stream, false);
	}
	oslo::bufferstream stream (bytes.data (), bytes.size ());
	auto error (false);
	oslo::message_header header (error, stream);
	ASSERT_FALSE (error);
	ASSERT_EQ (oslo::message_type::keepalive, header.type);
	oslo::keepalive message2 (error, stream, header);
	ASSERT_FALSE (error);
	ASSERT_EQ (message1.peers, message2.peers);
}

TEST (message, publish_serialization)
{
	oslo::network_params params;
	oslo::publish publish (std::make_shared<oslo::send_block> (0, 1, 2, oslo::keypair ().prv, 4, 5));
	ASSERT_EQ (oslo::block_type::send, publish.header.block_type ());
	std::vector<uint8_t> bytes;
	{
		oslo::vectorstream stream (bytes);
		publish.header.serialize (stream, false);
	}
	ASSERT_EQ (8, bytes.size ());
	ASSERT_EQ (0x52, bytes[0]);
	ASSERT_EQ (0x41, bytes[1]);
	ASSERT_EQ (params.protocol.protocol_version, bytes[2]);
	ASSERT_EQ (params.protocol.protocol_version, bytes[3]);
	ASSERT_EQ (params.protocol.protocol_version_min (false), bytes[4]);
	ASSERT_EQ (static_cast<uint8_t> (oslo::message_type::publish), bytes[5]);
	ASSERT_EQ (0x00, bytes[6]); // extensions
	ASSERT_EQ (static_cast<uint8_t> (oslo::block_type::send), bytes[7]);
	oslo::bufferstream stream (bytes.data (), bytes.size ());
	auto error (false);
	oslo::message_header header (error, stream);
	ASSERT_FALSE (error);
	ASSERT_EQ (params.protocol.protocol_version_min (false), header.version_min ());
	ASSERT_EQ (params.protocol.protocol_version, header.version_using);
	ASSERT_EQ (params.protocol.protocol_version, header.version_max);
	ASSERT_EQ (oslo::message_type::publish, header.type);
}

TEST (message, confirm_ack_serialization)
{
	oslo::keypair key1;
	auto vote (std::make_shared<oslo::vote> (key1.pub, key1.prv, 0, std::make_shared<oslo::send_block> (0, 1, 2, key1.prv, 4, 5)));
	oslo::confirm_ack con1 (vote);
	std::vector<uint8_t> bytes;
	{
		oslo::vectorstream stream1 (bytes);
		con1.serialize (stream1, false);
	}
	oslo::bufferstream stream2 (bytes.data (), bytes.size ());
	bool error (false);
	oslo::message_header header (error, stream2);
	oslo::confirm_ack con2 (error, stream2, header);
	ASSERT_FALSE (error);
	ASSERT_EQ (con1, con2);
	ASSERT_EQ (header.block_type (), oslo::block_type::send);
}

TEST (message, confirm_ack_hash_serialization)
{
	std::vector<oslo::block_hash> hashes;
	for (auto i (hashes.size ()); i < oslo::network::confirm_ack_hashes_max; i++)
	{
		oslo::keypair key1;
		oslo::block_hash previous;
		oslo::random_pool::generate_block (previous.bytes.data (), previous.bytes.size ());
		oslo::state_block block (key1.pub, previous, key1.pub, 2, 4, key1.prv, key1.pub, 5);
		hashes.push_back (block.hash ());
	}
	oslo::keypair representative1;
	auto vote (std::make_shared<oslo::vote> (representative1.pub, representative1.prv, 0, hashes));
	oslo::confirm_ack con1 (vote);
	std::vector<uint8_t> bytes;
	{
		oslo::vectorstream stream1 (bytes);
		con1.serialize (stream1, false);
	}
	oslo::bufferstream stream2 (bytes.data (), bytes.size ());
	bool error (false);
	oslo::message_header header (error, stream2);
	oslo::confirm_ack con2 (error, stream2, header);
	ASSERT_FALSE (error);
	ASSERT_EQ (con1, con2);
	std::vector<oslo::block_hash> vote_blocks;
	for (auto block : con2.vote->blocks)
	{
		vote_blocks.push_back (boost::get<oslo::block_hash> (block));
	}
	ASSERT_EQ (hashes, vote_blocks);
	// Check overflow with max hashes
	ASSERT_EQ (header.count_get (), hashes.size ());
	ASSERT_EQ (header.block_type (), oslo::block_type::not_a_block);
}

TEST (message, confirm_req_serialization)
{
	oslo::keypair key1;
	oslo::keypair key2;
	auto block (std::make_shared<oslo::send_block> (0, key2.pub, 200, oslo::keypair ().prv, 2, 3));
	oslo::confirm_req req (block);
	std::vector<uint8_t> bytes;
	{
		oslo::vectorstream stream (bytes);
		req.serialize (stream, false);
	}
	auto error (false);
	oslo::bufferstream stream2 (bytes.data (), bytes.size ());
	oslo::message_header header (error, stream2);
	oslo::confirm_req req2 (error, stream2, header);
	ASSERT_FALSE (error);
	ASSERT_EQ (req, req2);
	ASSERT_EQ (*req.block, *req2.block);
}

TEST (message, confirm_req_hash_serialization)
{
	oslo::keypair key1;
	oslo::keypair key2;
	oslo::send_block block (1, key2.pub, 200, oslo::keypair ().prv, 2, 3);
	oslo::confirm_req req (block.hash (), block.root ());
	std::vector<uint8_t> bytes;
	{
		oslo::vectorstream stream (bytes);
		req.serialize (stream, false);
	}
	auto error (false);
	oslo::bufferstream stream2 (bytes.data (), bytes.size ());
	oslo::message_header header (error, stream2);
	oslo::confirm_req req2 (error, stream2, header);
	ASSERT_FALSE (error);
	ASSERT_EQ (req, req2);
	ASSERT_EQ (req.roots_hashes, req2.roots_hashes);
	ASSERT_EQ (header.block_type (), oslo::block_type::not_a_block);
	ASSERT_EQ (header.count_get (), req.roots_hashes.size ());
}

TEST (message, confirm_req_hash_batch_serialization)
{
	oslo::keypair key;
	oslo::keypair representative;
	std::vector<std::pair<oslo::block_hash, oslo::root>> roots_hashes;
	oslo::state_block open (key.pub, 0, representative.pub, 2, 4, key.prv, key.pub, 5);
	roots_hashes.push_back (std::make_pair (open.hash (), open.root ()));
	for (auto i (roots_hashes.size ()); i < 7; i++)
	{
		oslo::keypair key1;
		oslo::block_hash previous;
		oslo::random_pool::generate_block (previous.bytes.data (), previous.bytes.size ());
		oslo::state_block block (key1.pub, previous, representative.pub, 2, 4, key1.prv, key1.pub, 5);
		roots_hashes.push_back (std::make_pair (block.hash (), block.root ()));
	}
	roots_hashes.push_back (std::make_pair (open.hash (), open.root ()));
	oslo::confirm_req req (roots_hashes);
	std::vector<uint8_t> bytes;
	{
		oslo::vectorstream stream (bytes);
		req.serialize (stream, false);
	}
	auto error (false);
	oslo::bufferstream stream2 (bytes.data (), bytes.size ());
	oslo::message_header header (error, stream2);
	oslo::confirm_req req2 (error, stream2, header);
	ASSERT_FALSE (error);
	ASSERT_EQ (req, req2);
	ASSERT_EQ (req.roots_hashes, req2.roots_hashes);
	ASSERT_EQ (req.roots_hashes, roots_hashes);
	ASSERT_EQ (req2.roots_hashes, roots_hashes);
	ASSERT_EQ (header.block_type (), oslo::block_type::not_a_block);
	ASSERT_EQ (header.count_get (), req.roots_hashes.size ());
}
