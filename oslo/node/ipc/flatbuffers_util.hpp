#pragma once

#include <oslo/ipc_flatbuffers_lib/generated/flatbuffers/nanoapi_generated.h>

#include <memory>

namespace oslo
{
class amount;
class block;
class send_block;
class receive_block;
class change_block;
class open_block;
class state_block;
namespace ipc
{
	/**
	 * Utilities to convert between blocks and Flatbuffers equivalents
	 */
	class flatbuffers_builder
	{
	public:
		static osloapi::BlockUnion block_to_union (oslo::block const & block_a, oslo::amount const & amount_a, bool is_state_send_a = false);
		static std::unique_ptr<osloapi::BlockStateT> from (oslo::state_block const & block_a, oslo::amount const & amount_a, bool is_state_send_a);
		static std::unique_ptr<osloapi::BlockSendT> from (oslo::send_block const & block_a);
		static std::unique_ptr<osloapi::BlockReceiveT> from (oslo::receive_block const & block_a);
		static std::unique_ptr<osloapi::BlockOpenT> from (oslo::open_block const & block_a);
		static std::unique_ptr<osloapi::BlockChangeT> from (oslo::change_block const & block_a);
	};
}
}
