#include <oslo/node/testing.hpp>

#include <gtest/gtest.h>

namespace
{
class test_visitor : public oslo::message_visitor
{
public:
	void keepalive (oslo::keepalive const &) override
	{
		++keepalive_count;
	}
	void publish (oslo::publish const &) override
	{
		++publish_count;
	}
	void confirm_req (oslo::confirm_req const &) override
	{
		++confirm_req_count;
	}
	void confirm_ack (oslo::confirm_ack const &) override
	{
		++confirm_ack_count;
	}
	void bulk_pull (oslo::bulk_pull const &) override
	{
		ASSERT_FALSE (true);
	}
	void bulk_pull_account (oslo::bulk_pull_account const &) override
	{
		ASSERT_FALSE (true);
	}
	void bulk_push (oslo::bulk_push const &) override
	{
		ASSERT_FALSE (true);
	}
	void frontier_req (oslo::frontier_req const &) override
	{
		ASSERT_FALSE (true);
	}
	void node_id_handshake (oslo::node_id_handshake const &) override
	{
		ASSERT_FALSE (true);
	}
	void telemetry_req (oslo::telemetry_req const &) override
	{
		ASSERT_FALSE (true);
	}
	void telemetry_ack (oslo::telemetry_ack const &) override
	{
		ASSERT_FALSE (true);
	}

	uint64_t keepalive_count{ 0 };
	uint64_t publish_count{ 0 };
	uint64_t confirm_req_count{ 0 };
	uint64_t confirm_ack_count{ 0 };
};
}

TEST (message_parser, exact_confirm_ack_size)
{
	oslo::system system (1);
	test_visitor visitor;
	oslo::network_filter filter (1);
	oslo::block_uniquer block_uniquer;
	oslo::vote_uniquer vote_uniquer (block_uniquer);
	oslo::message_parser parser (filter, block_uniquer, vote_uniquer, visitor, system.work, false);
	auto block (std::make_shared<oslo::send_block> (1, 1, 2, oslo::keypair ().prv, 4, *system.work.generate (oslo::root (1))));
	auto vote (std::make_shared<oslo::vote> (0, oslo::keypair ().prv, 0, std::move (block)));
	oslo::confirm_ack message (vote);
	std::vector<uint8_t> bytes;
	{
		oslo::vectorstream stream (bytes);
		message.serialize (stream, true);
	}
	ASSERT_EQ (0, visitor.confirm_ack_count);
	ASSERT_EQ (parser.status, oslo::message_parser::parse_status::success);
	auto error (false);
	oslo::bufferstream stream1 (bytes.data (), bytes.size ());
	oslo::message_header header1 (error, stream1);
	ASSERT_FALSE (error);
	parser.deserialize_confirm_ack (stream1, header1);
	ASSERT_EQ (1, visitor.confirm_ack_count);
	ASSERT_EQ (parser.status, oslo::message_parser::parse_status::success);
	bytes.push_back (0);
	oslo::bufferstream stream2 (bytes.data (), bytes.size ());
	oslo::message_header header2 (error, stream2);
	ASSERT_FALSE (error);
	parser.deserialize_confirm_ack (stream2, header2);
	ASSERT_EQ (1, visitor.confirm_ack_count);
	ASSERT_NE (parser.status, oslo::message_parser::parse_status::success);
}

TEST (message_parser, exact_confirm_req_size)
{
	oslo::system system (1);
	test_visitor visitor;
	oslo::network_filter filter (1);
	oslo::block_uniquer block_uniquer;
	oslo::vote_uniquer vote_uniquer (block_uniquer);
	oslo::message_parser parser (filter, block_uniquer, vote_uniquer, visitor, system.work, false);
	auto block (std::make_shared<oslo::send_block> (1, 1, 2, oslo::keypair ().prv, 4, *system.work.generate (oslo::root (1))));
	oslo::confirm_req message (std::move (block));
	std::vector<uint8_t> bytes;
	{
		oslo::vectorstream stream (bytes);
		message.serialize (stream, false);
	}
	ASSERT_EQ (0, visitor.confirm_req_count);
	ASSERT_EQ (parser.status, oslo::message_parser::parse_status::success);
	auto error (false);
	oslo::bufferstream stream1 (bytes.data (), bytes.size ());
	oslo::message_header header1 (error, stream1);
	ASSERT_FALSE (error);
	parser.deserialize_confirm_req (stream1, header1);
	ASSERT_EQ (1, visitor.confirm_req_count);
	ASSERT_EQ (parser.status, oslo::message_parser::parse_status::success);
	bytes.push_back (0);
	oslo::bufferstream stream2 (bytes.data (), bytes.size ());
	oslo::message_header header2 (error, stream2);
	ASSERT_FALSE (error);
	parser.deserialize_confirm_req (stream2, header2);
	ASSERT_EQ (1, visitor.confirm_req_count);
	ASSERT_NE (parser.status, oslo::message_parser::parse_status::success);
}

TEST (message_parser, exact_confirm_req_hash_size)
{
	oslo::system system (1);
	test_visitor visitor;
	oslo::network_filter filter (1);
	oslo::block_uniquer block_uniquer;
	oslo::vote_uniquer vote_uniquer (block_uniquer);
	oslo::message_parser parser (filter, block_uniquer, vote_uniquer, visitor, system.work, true);
	oslo::send_block block (1, 1, 2, oslo::keypair ().prv, 4, *system.work.generate (oslo::root (1)));
	oslo::confirm_req message (block.hash (), block.root ());
	std::vector<uint8_t> bytes;
	{
		oslo::vectorstream stream (bytes);
		message.serialize (stream, false);
	}
	ASSERT_EQ (0, visitor.confirm_req_count);
	ASSERT_EQ (parser.status, oslo::message_parser::parse_status::success);
	auto error (false);
	oslo::bufferstream stream1 (bytes.data (), bytes.size ());
	oslo::message_header header1 (error, stream1);
	ASSERT_FALSE (error);
	parser.deserialize_confirm_req (stream1, header1);
	ASSERT_EQ (1, visitor.confirm_req_count);
	ASSERT_EQ (parser.status, oslo::message_parser::parse_status::success);
	bytes.push_back (0);
	oslo::bufferstream stream2 (bytes.data (), bytes.size ());
	oslo::message_header header2 (error, stream2);
	ASSERT_FALSE (error);
	parser.deserialize_confirm_req (stream2, header2);
	ASSERT_EQ (1, visitor.confirm_req_count);
	ASSERT_NE (parser.status, oslo::message_parser::parse_status::success);
}

TEST (message_parser, exact_publish_size)
{
	oslo::system system (1);
	test_visitor visitor;
	oslo::network_filter filter (1);
	oslo::block_uniquer block_uniquer;
	oslo::vote_uniquer vote_uniquer (block_uniquer);
	oslo::message_parser parser (filter, block_uniquer, vote_uniquer, visitor, system.work, true);
	auto block (std::make_shared<oslo::send_block> (1, 1, 2, oslo::keypair ().prv, 4, *system.work.generate (oslo::root (1))));
	oslo::publish message (std::move (block));
	std::vector<uint8_t> bytes;
	{
		oslo::vectorstream stream (bytes);
		message.serialize (stream, false);
	}
	ASSERT_EQ (0, visitor.publish_count);
	ASSERT_EQ (parser.status, oslo::message_parser::parse_status::success);
	auto error (false);
	oslo::bufferstream stream1 (bytes.data (), bytes.size ());
	oslo::message_header header1 (error, stream1);
	ASSERT_FALSE (error);
	parser.deserialize_publish (stream1, header1);
	ASSERT_EQ (1, visitor.publish_count);
	ASSERT_EQ (parser.status, oslo::message_parser::parse_status::success);
	bytes.push_back (0);
	oslo::bufferstream stream2 (bytes.data (), bytes.size ());
	oslo::message_header header2 (error, stream2);
	ASSERT_FALSE (error);
	parser.deserialize_publish (stream2, header2);
	ASSERT_EQ (1, visitor.publish_count);
	ASSERT_NE (parser.status, oslo::message_parser::parse_status::success);
}

TEST (message_parser, exact_keepalive_size)
{
	oslo::system system (1);
	test_visitor visitor;
	oslo::network_filter filter (1);
	oslo::block_uniquer block_uniquer;
	oslo::vote_uniquer vote_uniquer (block_uniquer);
	oslo::message_parser parser (filter, block_uniquer, vote_uniquer, visitor, system.work, true);
	oslo::keepalive message;
	std::vector<uint8_t> bytes;
	{
		oslo::vectorstream stream (bytes);
		message.serialize (stream, true);
	}
	ASSERT_EQ (0, visitor.keepalive_count);
	ASSERT_EQ (parser.status, oslo::message_parser::parse_status::success);
	auto error (false);
	oslo::bufferstream stream1 (bytes.data (), bytes.size ());
	oslo::message_header header1 (error, stream1);
	ASSERT_FALSE (error);
	parser.deserialize_keepalive (stream1, header1);
	ASSERT_EQ (1, visitor.keepalive_count);
	ASSERT_EQ (parser.status, oslo::message_parser::parse_status::success);
	bytes.push_back (0);
	oslo::bufferstream stream2 (bytes.data (), bytes.size ());
	oslo::message_header header2 (error, stream2);
	ASSERT_FALSE (error);
	parser.deserialize_keepalive (stream2, header2);
	ASSERT_EQ (1, visitor.keepalive_count);
	ASSERT_NE (parser.status, oslo::message_parser::parse_status::success);
}
