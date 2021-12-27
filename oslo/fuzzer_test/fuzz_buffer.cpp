#include <oslo/core_test/testutil.hpp>
#include <oslo/node/common.hpp>
#include <oslo/node/testing.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace oslo
{
void force_oslo_test_network ();
}
namespace
{
std::shared_ptr<oslo::system> system0;
std::shared_ptr<oslo::node> node0;

class fuzz_visitor : public oslo::message_visitor
{
public:
	virtual void keepalive (oslo::keepalive const &) override
	{
	}
	virtual void publish (oslo::publish const &) override
	{
	}
	virtual void confirm_req (oslo::confirm_req const &) override
	{
	}
	virtual void confirm_ack (oslo::confirm_ack const &) override
	{
	}
	virtual void bulk_pull (oslo::bulk_pull const &) override
	{
	}
	virtual void bulk_pull_account (oslo::bulk_pull_account const &) override
	{
	}
	virtual void bulk_push (oslo::bulk_push const &) override
	{
	}
	virtual void frontier_req (oslo::frontier_req const &) override
	{
	}
	virtual void node_id_handshake (oslo::node_id_handshake const &) override
	{
	}
	virtual void telemetry_req (oslo::telemetry_req const &) override
	{
	}
	virtual void telemetry_ack (oslo::telemetry_ack const &) override
	{
	}
};
}

/** Fuzz live message parsing. This covers parsing and block/vote uniquing. */
void fuzz_message_parser (const uint8_t * Data, size_t Size)
{
	static bool initialized = false;
	if (!initialized)
	{
		oslo::force_oslo_test_network ();
		initialized = true;
		system0 = std::make_shared<oslo::system> (1);
		node0 = system0->nodes[0];
	}

	fuzz_visitor visitor;
	oslo::message_parser parser (node0->network.publish_filter, node0->block_uniquer, node0->vote_uniquer, visitor, node0->work);
	parser.deserialize_buffer (Data, Size);
}

/** Fuzzer entry point */
extern "C" int LLVMFuzzerTestOneInput (const uint8_t * Data, size_t Size)
{
	fuzz_message_parser (Data, Size);
	return 0;
}
