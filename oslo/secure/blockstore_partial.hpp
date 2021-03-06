#pragma once

#include <oslo/lib/rep_weights.hpp>
#include <oslo/secure/blockstore.hpp>
#include <oslo/secure/buffer.hpp>

#include <crypto/cryptopp/words.h>

namespace oslo
{
template <typename Val, typename Derived_Store>
class block_predecessor_set;

/** This base class implements the block_store interface functions which have DB agnostic functionality */
template <typename Val, typename Derived_Store>
class block_store_partial : public block_store
{
public:
	using block_store::block_exists;
	using block_store::unchecked_put;

	friend class oslo::block_predecessor_set<Val, Derived_Store>;

	std::mutex cache_mutex;

	/**
	 * If using a different store version than the latest then you may need
	 * to modify some of the objects in the store to be appropriate for the version before an upgrade.
	 */
	void initialize (oslo::write_transaction const & transaction_a, oslo::genesis const & genesis_a, oslo::ledger_cache & ledger_cache_a) override
	{
		auto hash_l (genesis_a.hash ());
		debug_assert (latest_begin (transaction_a) == latest_end ());
		genesis_a.open->sideband_set (oslo::block_sideband (network_params.ledger.genesis_account, 0, network_params.ledger.genesis_amount, 1, oslo::seconds_since_epoch (), oslo::epoch::epoch_0, false, false, false));
		block_put (transaction_a, hash_l, *genesis_a.open);
		++ledger_cache_a.block_count;
		confirmation_height_put (transaction_a, network_params.ledger.genesis_account, oslo::confirmation_height_info{ 1, genesis_a.hash () });
		++ledger_cache_a.cemented_count;
		account_put (transaction_a, network_params.ledger.genesis_account, { hash_l, network_params.ledger.genesis_account, genesis_a.open->hash (), std::numeric_limits<oslo::uint128_t>::max (), oslo::seconds_since_epoch (), 1, oslo::epoch::epoch_0 });
		++ledger_cache_a.account_count;
		ledger_cache_a.rep_weights.representation_put (network_params.ledger.genesis_account, std::numeric_limits<oslo::uint128_t>::max ());
		frontier_put (transaction_a, hash_l, network_params.ledger.genesis_account);
	}

	oslo::uint128_t block_balance (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a) override
	{
		auto block (block_get (transaction_a, hash_a));
		release_assert (block);
		oslo::uint128_t result (block_balance_calculated (block));
		return result;
	}

	bool account_exists (oslo::transaction const & transaction_a, oslo::account const & account_a) override
	{
		auto iterator (latest_begin (transaction_a, account_a));
		return iterator != latest_end () && oslo::account (iterator->first) == account_a;
	}

	void confirmation_height_clear (oslo::write_transaction const & transaction_a, oslo::account const & account_a, uint64_t existing_confirmation_height_a) override
	{
		if (existing_confirmation_height_a > 0)
		{
			confirmation_height_put (transaction_a, account_a, { 0, oslo::block_hash{ 0 } });
		}
	}

	void confirmation_height_clear (oslo::write_transaction const & transaction_a) override
	{
		for (auto i (confirmation_height_begin (transaction_a)), n (confirmation_height_end ()); i != n; ++i)
		{
			confirmation_height_clear (transaction_a, i->first, i->second.height);
		}
	}

	bool pending_exists (oslo::transaction const & transaction_a, oslo::pending_key const & key_a) override
	{
		auto iterator (pending_begin (transaction_a, key_a));
		return iterator != pending_end () && oslo::pending_key (iterator->first) == key_a;
	}

	bool pending_any (oslo::transaction const & transaction_a, oslo::account const & account_a) override
	{
		auto iterator (pending_begin (transaction_a, oslo::pending_key (account_a, 0)));
		return iterator != pending_end () && oslo::pending_key (iterator->first).account == account_a;
	}

	bool unchecked_exists (oslo::transaction const & transaction_a, oslo::unchecked_key const & unchecked_key_a) override
	{
		oslo::db_val<Val> value;
		auto status (get (transaction_a, tables::unchecked, oslo::db_val<Val> (unchecked_key_a), value));
		release_assert (success (status) || not_found (status));
		return (success (status));
	}

	std::vector<oslo::unchecked_info> unchecked_get (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a) override
	{
		std::vector<oslo::unchecked_info> result;
		for (auto i (unchecked_begin (transaction_a, oslo::unchecked_key (hash_a, 0))), n (unchecked_end ()); i != n && i->first.key () == hash_a; ++i)
		{
			oslo::unchecked_info const & unchecked_info (i->second);
			result.push_back (unchecked_info);
		}
		return result;
	}

	void block_put (oslo::write_transaction const & transaction_a, oslo::block_hash const & hash_a, oslo::block const & block_a) override
	{
		debug_assert (block_a.sideband ().successor.is_zero () || block_exists (transaction_a, block_a.sideband ().successor));
		std::vector<uint8_t> vector;
		{
			oslo::vectorstream stream (vector);
			block_a.serialize (stream);
			block_a.sideband ().serialize (stream, block_a.type ());
		}
		block_raw_put (transaction_a, vector, block_a.type (), hash_a);
		oslo::block_predecessor_set<Val, Derived_Store> predecessor (transaction_a, *this);
		block_a.visit (predecessor);
		debug_assert (block_a.previous ().is_zero () || block_successor (transaction_a, block_a.previous ()) == hash_a);
	}

	// Converts a block hash to a block height
	uint64_t block_account_height (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a) const override
	{
		auto block = block_get (transaction_a, hash_a);
		debug_assert (block != nullptr);
		return block->sideband ().height;
	}

	std::shared_ptr<oslo::block> block_get (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a) const override
	{
		oslo::block_type type;
		auto value (block_raw_get (transaction_a, hash_a, type));
		std::shared_ptr<oslo::block> result;
		if (value.size () != 0)
		{
			oslo::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
			result = oslo::deserialize_block (stream, type);
			debug_assert (result != nullptr);
			oslo::block_sideband sideband;
			if (full_sideband (transaction_a) || entry_has_sideband (value.size (), type))
			{
				auto error (sideband.deserialize (stream, type));
				(void)error;
				debug_assert (!error);
			}
			else
			{
				// Reconstruct sideband data for block.
				sideband.account = block_account_computed (transaction_a, hash_a);
				sideband.balance = block_balance_computed (transaction_a, hash_a);
				sideband.successor = block_successor (transaction_a, hash_a);
				sideband.height = 0;
				sideband.timestamp = 0;
			}
			result->sideband_set (sideband);
		}
		return result;
	}

	std::shared_ptr<oslo::block> block_get_no_sideband (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a) const override
	{
		oslo::block_type type;
		auto value (block_raw_get (transaction_a, hash_a, type));
		std::shared_ptr<oslo::block> result;
		if (value.size () != 0)
		{
			oslo::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
			result = oslo::deserialize_block (stream, type);
			debug_assert (result != nullptr);
		}
		return result;
	}

	bool block_exists (oslo::transaction const & transaction_a, oslo::block_type type, oslo::block_hash const & hash_a) override
	{
		auto junk = block_raw_get_by_type (transaction_a, hash_a, type);
		return junk.is_initialized ();
	}

	bool block_exists (oslo::transaction const & tx_a, oslo::block_hash const & hash_a) override
	{
		// Table lookups are ordered by match probability
		// clang-format off
		return
			block_exists (tx_a, oslo::block_type::state, hash_a) ||
			block_exists (tx_a, oslo::block_type::send, hash_a) ||
			block_exists (tx_a, oslo::block_type::receive, hash_a) ||
			block_exists (tx_a, oslo::block_type::open, hash_a) ||
			block_exists (tx_a, oslo::block_type::change, hash_a);
		// clang-format on
	}

	bool root_exists (oslo::transaction const & transaction_a, oslo::root const & root_a) override
	{
		return block_exists (transaction_a, root_a) || account_exists (transaction_a, root_a);
	}

	bool source_exists (oslo::transaction const & transaction_a, oslo::block_hash const & source_a) override
	{
		return block_exists (transaction_a, oslo::block_type::state, source_a) || block_exists (transaction_a, oslo::block_type::send, source_a);
	}

	oslo::account block_account (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a) const override
	{
		auto block (block_get (transaction_a, hash_a));
		debug_assert (block != nullptr);
		return block_account_calculated (*block);
	}

	oslo::account block_account_calculated (oslo::block const & block_a) const override
	{
		debug_assert (block_a.has_sideband ());
		oslo::account result (block_a.account ());
		if (result.is_zero ())
		{
			result = block_a.sideband ().account;
		}
		debug_assert (!result.is_zero ());
		return result;
	}

	oslo::uint128_t block_balance_calculated (std::shared_ptr<oslo::block> const & block_a) const override
	{
		oslo::uint128_t result;
		switch (block_a->type ())
		{
			case oslo::block_type::open:
			case oslo::block_type::receive:
			case oslo::block_type::change:
				result = block_a->sideband ().balance.number ();
				break;
			case oslo::block_type::send:
				result = boost::polymorphic_downcast<oslo::send_block *> (block_a.get ())->hashables.balance.number ();
				break;
			case oslo::block_type::state:
				result = boost::polymorphic_downcast<oslo::state_block *> (block_a.get ())->hashables.balance.number ();
				break;
			case oslo::block_type::invalid:
			case oslo::block_type::not_a_block:
				release_assert (false);
				break;
		}
		return result;
	}

	oslo::block_hash block_successor (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a) const override
	{
		oslo::block_type type;
		auto value (block_raw_get (transaction_a, hash_a, type));
		oslo::block_hash result;
		if (value.size () != 0)
		{
			debug_assert (value.size () >= result.bytes.size ());
			oslo::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()) + block_successor_offset (transaction_a, value.size (), type), result.bytes.size ());
			auto error (oslo::try_read (stream, result.bytes));
			(void)error;
			debug_assert (!error);
		}
		else
		{
			result.clear ();
		}
		return result;
	}

	bool full_sideband (oslo::transaction const & transaction_a) const
	{
		return version_get (transaction_a) > 12;
	}

	void block_successor_clear (oslo::write_transaction const & transaction_a, oslo::block_hash const & hash_a) override
	{
		oslo::block_type type;
		auto value (block_raw_get (transaction_a, hash_a, type));
		debug_assert (value.size () != 0);
		std::vector<uint8_t> data (static_cast<uint8_t *> (value.data ()), static_cast<uint8_t *> (value.data ()) + value.size ());
		std::fill_n (data.begin () + block_successor_offset (transaction_a, value.size (), type), sizeof (oslo::block_hash), uint8_t{ 0 });
		block_raw_put (transaction_a, data, type, hash_a);
	}

	void unchecked_put (oslo::write_transaction const & transaction_a, oslo::block_hash const & hash_a, std::shared_ptr<oslo::block> const & block_a) override
	{
		oslo::unchecked_key key (hash_a, block_a->hash ());
		oslo::unchecked_info info (block_a, block_a->account (), oslo::seconds_since_epoch (), oslo::signature_verification::unknown);
		unchecked_put (transaction_a, key, info);
	}

	std::shared_ptr<oslo::vote> vote_current (oslo::transaction const & transaction_a, oslo::account const & account_a) override
	{
		debug_assert (!cache_mutex.try_lock ());
		std::shared_ptr<oslo::vote> result;
		auto existing (vote_cache_l1.find (account_a));
		auto have_existing (true);
		if (existing == vote_cache_l1.end ())
		{
			existing = vote_cache_l2.find (account_a);
			if (existing == vote_cache_l2.end ())
			{
				have_existing = false;
			}
		}
		if (have_existing)
		{
			result = existing->second;
		}
		else
		{
			result = vote_get (transaction_a, account_a);
		}
		return result;
	}

	std::shared_ptr<oslo::vote> vote_generate (oslo::transaction const & transaction_a, oslo::account const & account_a, oslo::raw_key const & key_a, std::shared_ptr<oslo::block> block_a) override
	{
		oslo::lock_guard<std::mutex> lock (cache_mutex);
		auto result (vote_current (transaction_a, account_a));
		uint64_t sequence ((result ? result->sequence : 0) + 1);
		result = std::make_shared<oslo::vote> (account_a, key_a, sequence, block_a);
		vote_cache_l1[account_a] = result;
		return result;
	}

	std::shared_ptr<oslo::vote> vote_generate (oslo::transaction const & transaction_a, oslo::account const & account_a, oslo::raw_key const & key_a, std::vector<oslo::block_hash> blocks_a) override
	{
		oslo::lock_guard<std::mutex> lock (cache_mutex);
		auto result (vote_current (transaction_a, account_a));
		uint64_t sequence ((result ? result->sequence : 0) + 1);
		result = std::make_shared<oslo::vote> (account_a, key_a, sequence, blocks_a);
		vote_cache_l1[account_a] = result;
		return result;
	}

	std::shared_ptr<oslo::vote> vote_max (oslo::transaction const & transaction_a, std::shared_ptr<oslo::vote> vote_a) override
	{
		oslo::lock_guard<std::mutex> lock (cache_mutex);
		auto current (vote_current (transaction_a, vote_a->account));
		auto result (vote_a);
		if (current != nullptr && current->sequence > result->sequence)
		{
			result = current;
		}
		vote_cache_l1[vote_a->account] = result;
		return result;
	}

	oslo::store_iterator<oslo::unchecked_key, oslo::unchecked_info> unchecked_end () const override
	{
		return oslo::store_iterator<oslo::unchecked_key, oslo::unchecked_info> (nullptr);
	}

	oslo::store_iterator<oslo::account, std::shared_ptr<oslo::vote>> vote_end () override
	{
		return oslo::store_iterator<oslo::account, std::shared_ptr<oslo::vote>> (nullptr);
	}

	oslo::store_iterator<oslo::endpoint_key, oslo::no_value> peers_end () const override
	{
		return oslo::store_iterator<oslo::endpoint_key, oslo::no_value> (nullptr);
	}

	oslo::store_iterator<oslo::pending_key, oslo::pending_info> pending_end () override
	{
		return oslo::store_iterator<oslo::pending_key, oslo::pending_info> (nullptr);
	}

	oslo::store_iterator<uint64_t, oslo::amount> online_weight_end () const override
	{
		return oslo::store_iterator<uint64_t, oslo::amount> (nullptr);
	}

	oslo::store_iterator<oslo::account, oslo::account_info> latest_end () const override
	{
		return oslo::store_iterator<oslo::account, oslo::account_info> (nullptr);
	}

	oslo::store_iterator<oslo::account, oslo::confirmation_height_info> confirmation_height_end () override
	{
		return oslo::store_iterator<oslo::account, oslo::confirmation_height_info> (nullptr);
	}

	std::mutex & get_cache_mutex () override
	{
		return cache_mutex;
	}

	void block_del (oslo::write_transaction const & transaction_a, oslo::block_hash const & hash_a, oslo::block_type block_type_a) override
	{
		auto table = tables::state_blocks;
		switch (block_type_a)
		{
			case oslo::block_type::open:
				table = tables::open_blocks;
				break;
			case oslo::block_type::receive:
				table = tables::receive_blocks;
				break;
			case oslo::block_type::send:
				table = tables::send_blocks;
				break;
			case oslo::block_type::change:
				table = tables::change_blocks;
				break;
			case oslo::block_type::state:
				table = tables::state_blocks;
				break;
			default:
				debug_assert (false);
		}

		auto status = del (transaction_a, table, hash_a);
		release_assert (success (status));
	}

	int version_get (oslo::transaction const & transaction_a) const override
	{
		oslo::uint256_union version_key (1);
		oslo::db_val<Val> data;
		auto status = get (transaction_a, tables::meta, oslo::db_val<Val> (version_key), data);
		int result (1);
		if (!not_found (status))
		{
			oslo::uint256_union version_value (data);
			debug_assert (version_value.qwords[2] == 0 && version_value.qwords[1] == 0 && version_value.qwords[0] == 0);
			result = version_value.number ().convert_to<int> ();
		}
		return result;
	}

	oslo::epoch block_version (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a) override
	{
		oslo::db_val<Val> value;
		auto block = block_get (transaction_a, hash_a);
		if (block && block->type () == oslo::block_type::state)
		{
			return block->sideband ().details.epoch;
		}

		return oslo::epoch::epoch_0;
	}

	void block_raw_put (oslo::write_transaction const & transaction_a, std::vector<uint8_t> const & data, oslo::block_type block_type_a, oslo::block_hash const & hash_a)
	{
		auto database_a = block_database (block_type_a);
		oslo::db_val<Val> value{ data.size (), (void *)data.data () };
		auto status = put (transaction_a, database_a, hash_a, value);
		release_assert (success (status));
	}

	void pending_put (oslo::write_transaction const & transaction_a, oslo::pending_key const & key_a, oslo::pending_info const & pending_info_a) override
	{
		oslo::db_val<Val> pending (pending_info_a);
		auto status = put (transaction_a, tables::pending, key_a, pending);
		release_assert (success (status));
	}

	void pending_del (oslo::write_transaction const & transaction_a, oslo::pending_key const & key_a) override
	{
		auto status = del (transaction_a, tables::pending, key_a);
		release_assert (success (status));
	}

	bool pending_get (oslo::transaction const & transaction_a, oslo::pending_key const & key_a, oslo::pending_info & pending_a) override
	{
		oslo::db_val<Val> value;
		oslo::db_val<Val> key (key_a);
		auto status1 = get (transaction_a, tables::pending, key, value);
		release_assert (success (status1) || not_found (status1));
		bool result (true);
		if (success (status1))
		{
			oslo::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
			result = pending_a.deserialize (stream);
		}
		return result;
	}

	void frontier_put (oslo::write_transaction const & transaction_a, oslo::block_hash const & block_a, oslo::account const & account_a) override
	{
		oslo::db_val<Val> account (account_a);
		auto status (put (transaction_a, tables::frontiers, block_a, account));
		release_assert (success (status));
	}

	oslo::account frontier_get (oslo::transaction const & transaction_a, oslo::block_hash const & block_a) const override
	{
		oslo::db_val<Val> value;
		auto status (get (transaction_a, tables::frontiers, oslo::db_val<Val> (block_a), value));
		release_assert (success (status) || not_found (status));
		oslo::account result (0);
		if (success (status))
		{
			result = static_cast<oslo::account> (value);
		}
		return result;
	}

	void frontier_del (oslo::write_transaction const & transaction_a, oslo::block_hash const & block_a) override
	{
		auto status (del (transaction_a, tables::frontiers, block_a));
		release_assert (success (status));
	}

	void unchecked_put (oslo::write_transaction const & transaction_a, oslo::unchecked_key const & key_a, oslo::unchecked_info const & info_a) override
	{
		oslo::db_val<Val> info (info_a);
		auto status (put (transaction_a, tables::unchecked, key_a, info));
		release_assert (success (status));
	}

	void unchecked_del (oslo::write_transaction const & transaction_a, oslo::unchecked_key const & key_a) override
	{
		auto status (del (transaction_a, tables::unchecked, key_a));
		release_assert (success (status));
	}

	std::shared_ptr<oslo::vote> vote_get (oslo::transaction const & transaction_a, oslo::account const & account_a) override
	{
		oslo::db_val<Val> value;
		auto status (get (transaction_a, tables::vote, oslo::db_val<Val> (account_a), value));
		release_assert (success (status) || not_found (status));
		if (success (status))
		{
			std::shared_ptr<oslo::vote> result (value);
			debug_assert (result != nullptr);
			return result;
		}
		return nullptr;
	}

	void flush (oslo::write_transaction const & transaction_a) override
	{
		{
			oslo::lock_guard<std::mutex> lock (cache_mutex);
			vote_cache_l1.swap (vote_cache_l2);
			vote_cache_l1.clear ();
		}
		for (auto i (vote_cache_l2.begin ()), n (vote_cache_l2.end ()); i != n; ++i)
		{
			std::vector<uint8_t> vector;
			{
				oslo::vectorstream stream (vector);
				i->second->serialize (stream);
			}
			oslo::db_val<Val> value (vector.size (), vector.data ());
			auto status1 (put (transaction_a, tables::vote, i->first, value));
			release_assert (success (status1));
		}
	}

	void online_weight_put (oslo::write_transaction const & transaction_a, uint64_t time_a, oslo::amount const & amount_a) override
	{
		oslo::db_val<Val> value (amount_a);
		auto status (put (transaction_a, tables::online_weight, time_a, value));
		release_assert (success (status));
	}

	void online_weight_del (oslo::write_transaction const & transaction_a, uint64_t time_a) override
	{
		auto status (del (transaction_a, tables::online_weight, time_a));
		release_assert (success (status));
	}

	void account_put (oslo::write_transaction const & transaction_a, oslo::account const & account_a, oslo::account_info const & info_a) override
	{
		// Check we are still in sync with other tables
		debug_assert (confirmation_height_exists (transaction_a, account_a));
		oslo::db_val<Val> info (info_a);
		auto status = put (transaction_a, tables::accounts, account_a, info);
		release_assert (success (status));
	}

	void account_del (oslo::write_transaction const & transaction_a, oslo::account const & account_a) override
	{
		auto status = del (transaction_a, tables::accounts, account_a);
		release_assert (success (status));
	}

	bool account_get (oslo::transaction const & transaction_a, oslo::account const & account_a, oslo::account_info & info_a) override
	{
		oslo::db_val<Val> value;
		oslo::db_val<Val> account (account_a);
		auto status1 (get (transaction_a, tables::accounts, account, value));
		release_assert (success (status1) || not_found (status1));
		bool result (true);
		if (success (status1))
		{
			oslo::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
			result = info_a.deserialize (stream);
		}
		return result;
	}

	void unchecked_clear (oslo::write_transaction const & transaction_a) override
	{
		auto status = drop (transaction_a, tables::unchecked);
		release_assert (success (status));
	}

	size_t online_weight_count (oslo::transaction const & transaction_a) const override
	{
		return count (transaction_a, tables::online_weight);
	}

	void online_weight_clear (oslo::write_transaction const & transaction_a) override
	{
		auto status (drop (transaction_a, tables::online_weight));
		release_assert (success (status));
	}

	void peer_put (oslo::write_transaction const & transaction_a, oslo::endpoint_key const & endpoint_a) override
	{
		oslo::db_val<Val> zero (static_cast<uint64_t> (0));
		auto status = put (transaction_a, tables::peers, endpoint_a, zero);
		release_assert (success (status));
	}

	void peer_del (oslo::write_transaction const & transaction_a, oslo::endpoint_key const & endpoint_a) override
	{
		auto status (del (transaction_a, tables::peers, endpoint_a));
		release_assert (success (status));
	}

	bool peer_exists (oslo::transaction const & transaction_a, oslo::endpoint_key const & endpoint_a) const override
	{
		return exists (transaction_a, tables::peers, oslo::db_val<Val> (endpoint_a));
	}

	size_t peer_count (oslo::transaction const & transaction_a) const override
	{
		return count (transaction_a, tables::peers);
	}

	void peer_clear (oslo::write_transaction const & transaction_a) override
	{
		auto status = drop (transaction_a, tables::peers);
		release_assert (success (status));
	}

	bool exists (oslo::transaction const & transaction_a, tables table_a, oslo::db_val<Val> const & key_a) const
	{
		return static_cast<const Derived_Store &> (*this).exists (transaction_a, table_a, key_a);
	}

	oslo::block_counts block_count (oslo::transaction const & transaction_a) override
	{
		oslo::block_counts result;
		result.send = count (transaction_a, tables::send_blocks);
		result.receive = count (transaction_a, tables::receive_blocks);
		result.open = count (transaction_a, tables::open_blocks);
		result.change = count (transaction_a, tables::change_blocks);
		result.state = count (transaction_a, tables::state_blocks);
		return result;
	}

	size_t account_count (oslo::transaction const & transaction_a) override
	{
		return count (transaction_a, tables::accounts);
	}

	std::shared_ptr<oslo::block> block_random (oslo::transaction const & transaction_a) override
	{
		auto count (block_count (transaction_a));
		release_assert (std::numeric_limits<CryptoPP::word32>::max () > count.sum ());
		auto region = static_cast<size_t> (oslo::random_pool::generate_word32 (0, static_cast<CryptoPP::word32> (count.sum () - 1)));
		std::shared_ptr<oslo::block> result;
		auto & derived_store = static_cast<Derived_Store &> (*this);
		if (region < count.send)
		{
			result = derived_store.template block_random<oslo::send_block> (transaction_a, tables::send_blocks);
		}
		else
		{
			region -= count.send;
			if (region < count.receive)
			{
				result = derived_store.template block_random<oslo::receive_block> (transaction_a, tables::receive_blocks);
			}
			else
			{
				region -= count.receive;
				if (region < count.open)
				{
					result = derived_store.template block_random<oslo::open_block> (transaction_a, tables::open_blocks);
				}
				else
				{
					region -= count.open;
					if (region < count.change)
					{
						result = derived_store.template block_random<oslo::change_block> (transaction_a, tables::change_blocks);
					}
					else
					{
						result = derived_store.template block_random<oslo::state_block> (transaction_a, tables::state_blocks);
					}
				}
			}
		}
		debug_assert (result != nullptr);
		return result;
	}

	uint64_t confirmation_height_count (oslo::transaction const & transaction_a) override
	{
		return count (transaction_a, tables::confirmation_height);
	}

	void confirmation_height_put (oslo::write_transaction const & transaction_a, oslo::account const & account_a, oslo::confirmation_height_info const & confirmation_height_info_a) override
	{
		oslo::db_val<Val> confirmation_height_info (confirmation_height_info_a);
		auto status = put (transaction_a, tables::confirmation_height, account_a, confirmation_height_info);
		release_assert (success (status));
	}

	bool confirmation_height_get (oslo::transaction const & transaction_a, oslo::account const & account_a, oslo::confirmation_height_info & confirmation_height_info_a) override
	{
		oslo::db_val<Val> value;
		auto status = get (transaction_a, tables::confirmation_height, oslo::db_val<Val> (account_a), value);
		release_assert (success (status) || not_found (status));
		bool result (true);
		if (success (status))
		{
			oslo::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
			result = confirmation_height_info_a.deserialize (stream);
		}
		return result;
	}

	void confirmation_height_del (oslo::write_transaction const & transaction_a, oslo::account const & account_a) override
	{
		auto status (del (transaction_a, tables::confirmation_height, oslo::db_val<Val> (account_a)));
		release_assert (success (status));
	}

	bool confirmation_height_exists (oslo::transaction const & transaction_a, oslo::account const & account_a) const override
	{
		return exists (transaction_a, tables::confirmation_height, oslo::db_val<Val> (account_a));
	}

	oslo::store_iterator<oslo::account, oslo::account_info> latest_begin (oslo::transaction const & transaction_a, oslo::account const & account_a) const override
	{
		return make_iterator<oslo::account, oslo::account_info> (transaction_a, tables::accounts, oslo::db_val<Val> (account_a));
	}

	oslo::store_iterator<oslo::account, oslo::account_info> latest_begin (oslo::transaction const & transaction_a) const override
	{
		return make_iterator<oslo::account, oslo::account_info> (transaction_a, tables::accounts);
	}

	oslo::store_iterator<oslo::pending_key, oslo::pending_info> pending_begin (oslo::transaction const & transaction_a, oslo::pending_key const & key_a) override
	{
		return make_iterator<oslo::pending_key, oslo::pending_info> (transaction_a, tables::pending, oslo::db_val<Val> (key_a));
	}

	oslo::store_iterator<oslo::pending_key, oslo::pending_info> pending_begin (oslo::transaction const & transaction_a) override
	{
		return make_iterator<oslo::pending_key, oslo::pending_info> (transaction_a, tables::pending);
	}

	oslo::store_iterator<oslo::unchecked_key, oslo::unchecked_info> unchecked_begin (oslo::transaction const & transaction_a) const override
	{
		return make_iterator<oslo::unchecked_key, oslo::unchecked_info> (transaction_a, tables::unchecked);
	}

	oslo::store_iterator<oslo::unchecked_key, oslo::unchecked_info> unchecked_begin (oslo::transaction const & transaction_a, oslo::unchecked_key const & key_a) const override
	{
		return make_iterator<oslo::unchecked_key, oslo::unchecked_info> (transaction_a, tables::unchecked, oslo::db_val<Val> (key_a));
	}

	oslo::store_iterator<oslo::account, std::shared_ptr<oslo::vote>> vote_begin (oslo::transaction const & transaction_a) override
	{
		return make_iterator<oslo::account, std::shared_ptr<oslo::vote>> (transaction_a, tables::vote);
	}

	oslo::store_iterator<uint64_t, oslo::amount> online_weight_begin (oslo::transaction const & transaction_a) const override
	{
		return make_iterator<uint64_t, oslo::amount> (transaction_a, tables::online_weight);
	}

	oslo::store_iterator<oslo::endpoint_key, oslo::no_value> peers_begin (oslo::transaction const & transaction_a) const override
	{
		return make_iterator<oslo::endpoint_key, oslo::no_value> (transaction_a, tables::peers);
	}

	oslo::store_iterator<oslo::account, oslo::confirmation_height_info> confirmation_height_begin (oslo::transaction const & transaction_a, oslo::account const & account_a) override
	{
		return make_iterator<oslo::account, oslo::confirmation_height_info> (transaction_a, tables::confirmation_height, oslo::db_val<Val> (account_a));
	}

	oslo::store_iterator<oslo::account, oslo::confirmation_height_info> confirmation_height_begin (oslo::transaction const & transaction_a) override
	{
		return make_iterator<oslo::account, oslo::confirmation_height_info> (transaction_a, tables::confirmation_height);
	}

	size_t unchecked_count (oslo::transaction const & transaction_a) override
	{
		return count (transaction_a, tables::unchecked);
	}

protected:
	oslo::network_params network_params;
	std::unordered_map<oslo::account, std::shared_ptr<oslo::vote>> vote_cache_l1;
	std::unordered_map<oslo::account, std::shared_ptr<oslo::vote>> vote_cache_l2;
	static int constexpr version{ 18 };

	template <typename T>
	std::shared_ptr<oslo::block> block_random (oslo::transaction const & transaction_a, tables table_a)
	{
		oslo::block_hash hash;
		oslo::random_pool::generate_block (hash.bytes.data (), hash.bytes.size ());
		auto existing = make_iterator<oslo::block_hash, std::shared_ptr<T>> (transaction_a, table_a, oslo::db_val<Val> (hash));
		if (existing == oslo::store_iterator<oslo::block_hash, std::shared_ptr<T>> (nullptr))
		{
			existing = make_iterator<oslo::block_hash, std::shared_ptr<T>> (transaction_a, table_a);
		}
		auto end (oslo::store_iterator<oslo::block_hash, std::shared_ptr<T>> (nullptr));
		debug_assert (existing != end);
		return block_get (transaction_a, oslo::block_hash (existing->first));
	}

	template <typename Key, typename Value>
	oslo::store_iterator<Key, Value> make_iterator (oslo::transaction const & transaction_a, tables table_a) const
	{
		return static_cast<Derived_Store const &> (*this).template make_iterator<Key, Value> (transaction_a, table_a);
	}

	template <typename Key, typename Value>
	oslo::store_iterator<Key, Value> make_iterator (oslo::transaction const & transaction_a, tables table_a, oslo::db_val<Val> const & key) const
	{
		return static_cast<Derived_Store const &> (*this).template make_iterator<Key, Value> (transaction_a, table_a, key);
	}

	bool entry_has_sideband (size_t entry_size_a, oslo::block_type type_a) const
	{
		return entry_size_a == oslo::block::size (type_a) + oslo::block_sideband::size (type_a);
	}

	oslo::db_val<Val> block_raw_get (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a, oslo::block_type & type_a) const
	{
		oslo::db_val<Val> result;
		// Table lookups are ordered by match probability
		oslo::block_type block_types[]{ oslo::block_type::state, oslo::block_type::send, oslo::block_type::receive, oslo::block_type::open, oslo::block_type::change };
		for (auto current_type : block_types)
		{
			auto db_val (block_raw_get_by_type (transaction_a, hash_a, current_type));
			if (db_val.is_initialized ())
			{
				type_a = current_type;
				result = db_val.get ();
				break;
			}
		}

		return result;
	}

	// Return account containing hash
	oslo::account block_account_computed (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a) const
	{
		debug_assert (!full_sideband (transaction_a));
		oslo::account result (0);
		auto hash (hash_a);
		while (result.is_zero ())
		{
			auto block (block_get_no_sideband (transaction_a, hash));
			debug_assert (block);
			result = block->account ();
			if (result.is_zero ())
			{
				auto type (oslo::block_type::invalid);
				auto value (block_raw_get (transaction_a, block->previous (), type));
				if (entry_has_sideband (value.size (), type))
				{
					result = block_account (transaction_a, block->previous ());
				}
				else
				{
					oslo::block_info block_info;
					if (!block_info_get (transaction_a, hash, block_info))
					{
						result = block_info.account;
					}
					else
					{
						result = frontier_get (transaction_a, hash);
						if (result.is_zero ())
						{
							auto successor (block_successor (transaction_a, hash));
							debug_assert (!successor.is_zero ());
							hash = successor;
						}
					}
				}
			}
		}
		debug_assert (!result.is_zero ());
		return result;
	}

	oslo::uint128_t block_balance_computed (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a) const
	{
		debug_assert (!full_sideband (transaction_a));
		summation_visitor visitor (transaction_a, *this);
		return visitor.compute_balance (hash_a);
	}

	size_t block_successor_offset (oslo::transaction const & transaction_a, size_t entry_size_a, oslo::block_type type_a) const
	{
		size_t result;
		if (full_sideband (transaction_a) || entry_has_sideband (entry_size_a, type_a))
		{
			result = entry_size_a - oslo::block_sideband::size (type_a);
		}
		else
		{
			// Read old successor-only sideband
			debug_assert (entry_size_a == oslo::block::size (type_a) + sizeof (oslo::block_hash));
			result = entry_size_a - sizeof (oslo::block_hash);
		}
		return result;
	}

	boost::optional<oslo::db_val<Val>> block_raw_get_by_type (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a, oslo::block_type & type_a) const
	{
		oslo::db_val<Val> value;
		oslo::db_val<Val> hash (hash_a);
		int status = status_code_not_found ();
		switch (type_a)
		{
			case oslo::block_type::send:
			{
				status = get (transaction_a, tables::send_blocks, hash, value);
				break;
			}
			case oslo::block_type::receive:
			{
				status = get (transaction_a, tables::receive_blocks, hash, value);
				break;
			}
			case oslo::block_type::open:
			{
				status = get (transaction_a, tables::open_blocks, hash, value);
				break;
			}
			case oslo::block_type::change:
			{
				status = get (transaction_a, tables::change_blocks, hash, value);
				break;
			}
			case oslo::block_type::state:
			{
				status = get (transaction_a, tables::state_blocks, hash, value);
				break;
			}
			case oslo::block_type::invalid:
			case oslo::block_type::not_a_block:
			{
				break;
			}
		}

		release_assert (success (status) || not_found (status));
		boost::optional<oslo::db_val<Val>> result;
		if (success (status))
		{
			result = value;
		}
		return result;
	}

	tables block_database (oslo::block_type type_a)
	{
		tables result = tables::frontiers;
		switch (type_a)
		{
			case oslo::block_type::send:
				result = tables::send_blocks;
				break;
			case oslo::block_type::receive:
				result = tables::receive_blocks;
				break;
			case oslo::block_type::open:
				result = tables::open_blocks;
				break;
			case oslo::block_type::change:
				result = tables::change_blocks;
				break;
			case oslo::block_type::state:
				result = tables::state_blocks;
				break;
			default:
				debug_assert (false);
				break;
		}
		return result;
	}

	size_t count (oslo::transaction const & transaction_a, std::initializer_list<tables> dbs_a) const
	{
		size_t total_count = 0;
		for (auto db : dbs_a)
		{
			total_count += count (transaction_a, db);
		}
		return total_count;
	}

	int get (oslo::transaction const & transaction_a, tables table_a, oslo::db_val<Val> const & key_a, oslo::db_val<Val> & value_a) const
	{
		return static_cast<Derived_Store const &> (*this).get (transaction_a, table_a, key_a, value_a);
	}

	int put (oslo::write_transaction const & transaction_a, tables table_a, oslo::db_val<Val> const & key_a, oslo::db_val<Val> const & value_a)
	{
		return static_cast<Derived_Store &> (*this).put (transaction_a, table_a, key_a, value_a);
	}

	int del (oslo::write_transaction const & transaction_a, tables table_a, oslo::db_val<Val> const & key_a)
	{
		return static_cast<Derived_Store &> (*this).del (transaction_a, table_a, key_a);
	}

	virtual size_t count (oslo::transaction const & transaction_a, tables table_a) const = 0;
	virtual int drop (oslo::write_transaction const & transaction_a, tables table_a) = 0;
	virtual bool not_found (int status) const = 0;
	virtual bool success (int status) const = 0;
	virtual int status_code_not_found () const = 0;
};

/**
 * Fill in our predecessors
 */
template <typename Val, typename Derived_Store>
class block_predecessor_set : public oslo::block_visitor
{
public:
	block_predecessor_set (oslo::write_transaction const & transaction_a, oslo::block_store_partial<Val, Derived_Store> & store_a) :
	transaction (transaction_a),
	store (store_a)
	{
	}
	virtual ~block_predecessor_set () = default;
	void fill_value (oslo::block const & block_a)
	{
		auto hash (block_a.hash ());
		oslo::block_type type;
		auto value (store.block_raw_get (transaction, block_a.previous (), type));
		debug_assert (value.size () != 0);
		std::vector<uint8_t> data (static_cast<uint8_t *> (value.data ()), static_cast<uint8_t *> (value.data ()) + value.size ());
		std::copy (hash.bytes.begin (), hash.bytes.end (), data.begin () + store.block_successor_offset (transaction, value.size (), type));
		store.block_raw_put (transaction, data, type, block_a.previous ());
	}
	void send_block (oslo::send_block const & block_a) override
	{
		fill_value (block_a);
	}
	void receive_block (oslo::receive_block const & block_a) override
	{
		fill_value (block_a);
	}
	void open_block (oslo::open_block const & block_a) override
	{
		// Open blocks don't have a predecessor
	}
	void change_block (oslo::change_block const & block_a) override
	{
		fill_value (block_a);
	}
	void state_block (oslo::state_block const & block_a) override
	{
		if (!block_a.previous ().is_zero ())
		{
			fill_value (block_a);
		}
	}
	oslo::write_transaction const & transaction;
	oslo::block_store_partial<Val, Derived_Store> & store;
};
}
