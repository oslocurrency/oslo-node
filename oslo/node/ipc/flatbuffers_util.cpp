#include <oslo/lib/blocks.hpp>
#include <oslo/lib/numbers.hpp>
#include <oslo/node/ipc/flatbuffers_util.hpp>
#include <oslo/secure/common.hpp>

std::unique_ptr<osloapi::BlockStateT> oslo::ipc::flatbuffers_builder::from (oslo::state_block const & block_a, oslo::amount const & amount_a, bool is_state_send_a)
{
	static oslo::network_params params;
	auto block (std::make_unique<osloapi::BlockStateT> ());
	block->account = block_a.account ().to_account ();
	block->hash = block_a.hash ().to_string ();
	block->previous = block_a.previous ().to_string ();
	block->representative = block_a.representative ().to_account ();
	block->balance = block_a.balance ().to_string_dec ();
	block->link = block_a.link ().to_string ();
	block->link_as_account = block_a.link ().to_account ();
	block_a.signature.encode_hex (block->signature);
	block->work = oslo::to_string_hex (block_a.work);

	if (is_state_send_a)
	{
		block->subtype = osloapi::BlockSubType::BlockSubType_send;
	}
	else if (block_a.link ().is_zero ())
	{
		block->subtype = osloapi::BlockSubType::BlockSubType_change;
	}
	else if (amount_a == 0 && params.ledger.epochs.is_epoch_link (block_a.link ()))
	{
		block->subtype = osloapi::BlockSubType::BlockSubType_epoch;
	}
	else
	{
		block->subtype = osloapi::BlockSubType::BlockSubType_receive;
	}
	return block;
}

std::unique_ptr<osloapi::BlockSendT> oslo::ipc::flatbuffers_builder::from (oslo::send_block const & block_a)
{
	auto block (std::make_unique<osloapi::BlockSendT> ());
	block->hash = block_a.hash ().to_string ();
	block->balance = block_a.balance ().to_string_dec ();
	block->destination = block_a.hashables.destination.to_account ();
	block->previous = block_a.previous ().to_string ();
	block_a.signature.encode_hex (block->signature);
	block->work = oslo::to_string_hex (block_a.work);
	return block;
}

std::unique_ptr<osloapi::BlockReceiveT> oslo::ipc::flatbuffers_builder::from (oslo::receive_block const & block_a)
{
	auto block (std::make_unique<osloapi::BlockReceiveT> ());
	block->hash = block_a.hash ().to_string ();
	block->source = block_a.source ().to_string ();
	block->previous = block_a.previous ().to_string ();
	block_a.signature.encode_hex (block->signature);
	block->work = oslo::to_string_hex (block_a.work);
	return block;
}

std::unique_ptr<osloapi::BlockOpenT> oslo::ipc::flatbuffers_builder::from (oslo::open_block const & block_a)
{
	auto block (std::make_unique<osloapi::BlockOpenT> ());
	block->hash = block_a.hash ().to_string ();
	block->source = block_a.source ().to_string ();
	block->account = block_a.account ().to_account ();
	block->representative = block_a.representative ().to_account ();
	block_a.signature.encode_hex (block->signature);
	block->work = oslo::to_string_hex (block_a.work);
	return block;
}

std::unique_ptr<osloapi::BlockChangeT> oslo::ipc::flatbuffers_builder::from (oslo::change_block const & block_a)
{
	auto block (std::make_unique<osloapi::BlockChangeT> ());
	block->hash = block_a.hash ().to_string ();
	block->previous = block_a.previous ().to_string ();
	block->representative = block_a.representative ().to_account ();
	block_a.signature.encode_hex (block->signature);
	block->work = oslo::to_string_hex (block_a.work);
	return block;
}

osloapi::BlockUnion oslo::ipc::flatbuffers_builder::block_to_union (oslo::block const & block_a, oslo::amount const & amount_a, bool is_state_send_a)
{
	osloapi::BlockUnion u;
	switch (block_a.type ())
	{
		case oslo::block_type::state:
		{
			u.Set (*from (dynamic_cast<oslo::state_block const &> (block_a), amount_a, is_state_send_a));
			break;
		}
		case oslo::block_type::send:
		{
			u.Set (*from (dynamic_cast<oslo::send_block const &> (block_a)));
			break;
		}
		case oslo::block_type::receive:
		{
			u.Set (*from (dynamic_cast<oslo::receive_block const &> (block_a)));
			break;
		}
		case oslo::block_type::open:
		{
			u.Set (*from (dynamic_cast<oslo::open_block const &> (block_a)));
			break;
		}
		case oslo::block_type::change:
		{
			u.Set (*from (dynamic_cast<oslo::change_block const &> (block_a)));
			break;
		}

		default:
			debug_assert (false);
	}
	return u;
}
