#include <oslo/lib/threading.hpp>
#include <oslo/secure/blockstore.hpp>

oslo::summation_visitor::summation_visitor (oslo::transaction const & transaction_a, oslo::block_store const & store_a, bool is_v14_upgrade_a) :
transaction (transaction_a),
store (store_a),
is_v14_upgrade (is_v14_upgrade_a)
{
}

void oslo::summation_visitor::send_block (oslo::send_block const & block_a)
{
	debug_assert (current->type != summation_type::invalid && current != nullptr);
	if (current->type == summation_type::amount)
	{
		sum_set (block_a.hashables.balance.number ());
		current->balance_hash = block_a.hashables.previous;
		current->amount_hash = 0;
	}
	else
	{
		sum_add (block_a.hashables.balance.number ());
		current->balance_hash = 0;
	}
}

void oslo::summation_visitor::state_block (oslo::state_block const & block_a)
{
	debug_assert (current->type != summation_type::invalid && current != nullptr);
	sum_set (block_a.hashables.balance.number ());
	if (current->type == summation_type::amount)
	{
		current->balance_hash = block_a.hashables.previous;
		current->amount_hash = 0;
	}
	else
	{
		current->balance_hash = 0;
	}
}

void oslo::summation_visitor::receive_block (oslo::receive_block const & block_a)
{
	debug_assert (current->type != summation_type::invalid && current != nullptr);
	if (current->type == summation_type::amount)
	{
		current->amount_hash = block_a.hashables.source;
	}
	else
	{
		oslo::block_info block_info;
		if (!store.block_info_get (transaction, block_a.hash (), block_info))
		{
			sum_add (block_info.balance.number ());
			current->balance_hash = 0;
		}
		else
		{
			current->amount_hash = block_a.hashables.source;
			current->balance_hash = block_a.hashables.previous;
		}
	}
}

void oslo::summation_visitor::open_block (oslo::open_block const & block_a)
{
	debug_assert (current->type != summation_type::invalid && current != nullptr);
	if (current->type == summation_type::amount)
	{
		if (block_a.hashables.source != network_params.ledger.genesis_account)
		{
			current->amount_hash = block_a.hashables.source;
		}
		else
		{
			sum_set (network_params.ledger.genesis_amount);
			current->amount_hash = 0;
		}
	}
	else
	{
		current->amount_hash = block_a.hashables.source;
		current->balance_hash = 0;
	}
}

void oslo::summation_visitor::change_block (oslo::change_block const & block_a)
{
	debug_assert (current->type != summation_type::invalid && current != nullptr);
	if (current->type == summation_type::amount)
	{
		sum_set (0);
		current->amount_hash = 0;
	}
	else
	{
		oslo::block_info block_info;
		if (!store.block_info_get (transaction, block_a.hash (), block_info))
		{
			sum_add (block_info.balance.number ());
			current->balance_hash = 0;
		}
		else
		{
			current->balance_hash = block_a.hashables.previous;
		}
	}
}

oslo::summation_visitor::frame oslo::summation_visitor::push (oslo::summation_visitor::summation_type type_a, oslo::block_hash const & hash_a)
{
	frames.emplace (type_a, type_a == summation_type::balance ? hash_a : 0, type_a == summation_type::amount ? hash_a : 0);
	return frames.top ();
}

void oslo::summation_visitor::sum_add (oslo::uint128_t addend_a)
{
	current->sum += addend_a;
	result = current->sum;
}

void oslo::summation_visitor::sum_set (oslo::uint128_t value_a)
{
	current->sum = value_a;
	result = current->sum;
}

oslo::uint128_t oslo::summation_visitor::compute_internal (oslo::summation_visitor::summation_type type_a, oslo::block_hash const & hash_a)
{
	push (type_a, hash_a);

	/*
	 Invocation loop representing balance and amount computations calling each other.
	 This is usually better done by recursion or something like boost::coroutine2, but
	 segmented stacks are not supported on all platforms so we do it manually to avoid
	 stack overflow (the mutual calls are not tail-recursive so we cannot rely on the
	 compiler optimizing that into a loop, though a future alternative is to do a
	 CPS-style implementation to enforce tail calls.)
	*/
	while (!frames.empty ())
	{
		current = &frames.top ();
		debug_assert (current->type != summation_type::invalid && current != nullptr);

		if (current->type == summation_type::balance)
		{
			if (current->awaiting_result)
			{
				sum_add (current->incoming_result);
				current->awaiting_result = false;
			}

			while (!current->awaiting_result && (!current->balance_hash.is_zero () || !current->amount_hash.is_zero ()))
			{
				if (!current->amount_hash.is_zero ())
				{
					// Compute amount
					current->awaiting_result = true;
					push (summation_type::amount, current->amount_hash);
					current->amount_hash = 0;
				}
				else
				{
					auto block (block_get (transaction, current->balance_hash));
					debug_assert (block != nullptr);
					block->visit (*this);
				}
			}

			epilogue ();
		}
		else if (current->type == summation_type::amount)
		{
			if (current->awaiting_result)
			{
				sum_set (current->sum < current->incoming_result ? current->incoming_result - current->sum : current->sum - current->incoming_result);
				current->awaiting_result = false;
			}

			while (!current->awaiting_result && (!current->amount_hash.is_zero () || !current->balance_hash.is_zero ()))
			{
				if (!current->amount_hash.is_zero ())
				{
					auto block = block_get (transaction, current->amount_hash);
					if (block != nullptr)
					{
						block->visit (*this);
					}
					else
					{
						if (current->amount_hash == network_params.ledger.genesis_account)
						{
							sum_set ((std::numeric_limits<oslo::uint128_t>::max) ());
							current->amount_hash = 0;
						}
						else
						{
							debug_assert (false);
							sum_set (0);
							current->amount_hash = 0;
						}
					}
				}
				else
				{
					// Compute balance
					current->awaiting_result = true;
					push (summation_type::balance, current->balance_hash);
					current->balance_hash = 0;
				}
			}

			epilogue ();
		}
	}

	return result;
}

void oslo::summation_visitor::epilogue ()
{
	if (!current->awaiting_result)
	{
		frames.pop ();
		if (!frames.empty ())
		{
			frames.top ().incoming_result = current->sum;
		}
	}
}

oslo::uint128_t oslo::summation_visitor::compute_amount (oslo::block_hash const & block_hash)
{
	return compute_internal (summation_type::amount, block_hash);
}

oslo::uint128_t oslo::summation_visitor::compute_balance (oslo::block_hash const & block_hash)
{
	return compute_internal (summation_type::balance, block_hash);
}

std::shared_ptr<oslo::block> oslo::summation_visitor::block_get (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a) const
{
	return is_v14_upgrade ? store.block_get_v14 (transaction, hash_a) : store.block_get_no_sideband (transaction, hash_a);
}

oslo::representative_visitor::representative_visitor (oslo::transaction const & transaction_a, oslo::block_store & store_a) :
transaction (transaction_a),
store (store_a),
result (0)
{
}

void oslo::representative_visitor::compute (oslo::block_hash const & hash_a)
{
	current = hash_a;
	while (result.is_zero ())
	{
		auto block (store.block_get (transaction, current));
		debug_assert (block != nullptr);
		block->visit (*this);
	}
}

void oslo::representative_visitor::send_block (oslo::send_block const & block_a)
{
	current = block_a.previous ();
}

void oslo::representative_visitor::receive_block (oslo::receive_block const & block_a)
{
	current = block_a.previous ();
}

void oslo::representative_visitor::open_block (oslo::open_block const & block_a)
{
	result = block_a.hash ();
}

void oslo::representative_visitor::change_block (oslo::change_block const & block_a)
{
	result = block_a.hash ();
}

void oslo::representative_visitor::state_block (oslo::state_block const & block_a)
{
	result = block_a.hash ();
}

oslo::read_transaction::read_transaction (std::unique_ptr<oslo::read_transaction_impl> read_transaction_impl) :
impl (std::move (read_transaction_impl))
{
}

void * oslo::read_transaction::get_handle () const
{
	return impl->get_handle ();
}

void oslo::read_transaction::reset () const
{
	impl->reset ();
}

void oslo::read_transaction::renew () const
{
	impl->renew ();
}

void oslo::read_transaction::refresh () const
{
	reset ();
	renew ();
}

oslo::write_transaction::write_transaction (std::unique_ptr<oslo::write_transaction_impl> write_transaction_impl) :
impl (std::move (write_transaction_impl))
{
	/*
	 * For IO threads, we do not want them to block on creating write transactions.
	 */
	debug_assert (oslo::thread_role::get () != oslo::thread_role::name::io);
}

void * oslo::write_transaction::get_handle () const
{
	return impl->get_handle ();
}

void oslo::write_transaction::commit () const
{
	impl->commit ();
}

void oslo::write_transaction::renew ()
{
	impl->renew ();
}

bool oslo::write_transaction::contains (oslo::tables table_a) const
{
	return impl->contains (table_a);
}
