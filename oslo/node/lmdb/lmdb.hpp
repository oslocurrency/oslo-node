#pragma once

#include <oslo/lib/diagnosticsconfig.hpp>
#include <oslo/lib/lmdbconfig.hpp>
#include <oslo/lib/logger_mt.hpp>
#include <oslo/lib/numbers.hpp>
#include <oslo/node/lmdb/lmdb_env.hpp>
#include <oslo/node/lmdb/lmdb_iterator.hpp>
#include <oslo/node/lmdb/lmdb_txn.hpp>
#include <oslo/secure/blockstore_partial.hpp>
#include <oslo/secure/common.hpp>
#include <oslo/secure/versioning.hpp>

#include <boost/optional.hpp>

#include <lmdb/libraries/liblmdb/lmdb.h>

namespace boost
{
namespace filesystem
{
	class path;
}
}

namespace oslo
{
using mdb_val = db_val<MDB_val>;

class logging_mt;
/**
 * mdb implementation of the block store
 */
class mdb_store : public block_store_partial<MDB_val, mdb_store>
{
public:
	using block_store_partial::block_exists;
	using block_store_partial::unchecked_put;

	mdb_store (oslo::logger_mt &, boost::filesystem::path const &, oslo::txn_tracking_config const & txn_tracking_config_a = oslo::txn_tracking_config{}, std::chrono::milliseconds block_processor_batch_max_time_a = std::chrono::milliseconds (5000), oslo::lmdb_config const & lmdb_config_a = oslo::lmdb_config{}, size_t batch_size = 512, bool backup_before_upgrade = false);
	oslo::write_transaction tx_begin_write (std::vector<oslo::tables> const & tables_requiring_lock = {}, std::vector<oslo::tables> const & tables_no_lock = {}) override;
	oslo::read_transaction tx_begin_read () override;

	std::string vendor_get () const override;

	bool block_info_get (oslo::transaction const &, oslo::block_hash const &, oslo::block_info &) const override;

	void version_put (oslo::write_transaction const &, int) override;

	void serialize_mdb_tracker (boost::property_tree::ptree &, std::chrono::milliseconds, std::chrono::milliseconds) override;

	static void create_backup_file (oslo::mdb_env &, boost::filesystem::path const &, oslo::logger_mt &);

private:
	oslo::logger_mt & logger;
	bool error{ false };

public:
	oslo::mdb_env env;

	/**
	 * Maps head block to owning account
	 * oslo::block_hash -> oslo::account
	 */
	MDB_dbi frontiers{ 0 };

	/**
	 * Maps account v1 to account information, head, rep, open, balance, timestamp and block count. (Removed)
	 * oslo::account -> oslo::block_hash, oslo::block_hash, oslo::block_hash, oslo::amount, uint64_t, uint64_t
	 */
	MDB_dbi accounts_v0{ 0 };

	/**
	 * Maps account v0 to account information, head, rep, open, balance, timestamp and block count. (Removed)
	 * oslo::account -> oslo::block_hash, oslo::block_hash, oslo::block_hash, oslo::amount, uint64_t, uint64_t
	 */
	MDB_dbi accounts_v1{ 0 };

	/**
	 * Maps account v0 to account information, head, rep, open, balance, timestamp, block count and epoch. (Removed)
	 * oslo::account -> oslo::block_hash, oslo::block_hash, oslo::block_hash, oslo::amount, uint64_t, uint64_t, oslo::epoch
	 */
	MDB_dbi accounts{ 0 };

	/**
	 * Maps block hash to send block.
	 * oslo::block_hash -> oslo::send_block
	 */
	MDB_dbi send_blocks{ 0 };

	/**
	 * Maps block hash to receive block.
	 * oslo::block_hash -> oslo::receive_block
	 */
	MDB_dbi receive_blocks{ 0 };

	/**
	 * Maps block hash to open block.
	 * oslo::block_hash -> oslo::open_block
	 */
	MDB_dbi open_blocks{ 0 };

	/**
	 * Maps block hash to change block.
	 * oslo::block_hash -> oslo::change_block
	 */
	MDB_dbi change_blocks{ 0 };

	/**
	 * Maps block hash to v0 state block. (Removed)
	 * oslo::block_hash -> oslo::state_block
	 */
	MDB_dbi state_blocks_v0{ 0 };

	/**
	 * Maps block hash to v1 state block. (Removed)
	 * oslo::block_hash -> oslo::state_block
	 */
	MDB_dbi state_blocks_v1{ 0 };

	/**
	 * Maps block hash to state block.
	 * oslo::block_hash -> oslo::state_block
	 */
	MDB_dbi state_blocks{ 0 };

	/**
	 * Maps min_version 0 (destination account, pending block) to (source account, amount). (Removed)
	 * oslo::account, oslo::block_hash -> oslo::account, oslo::amount
	 */
	MDB_dbi pending_v0{ 0 };

	/**
	 * Maps min_version 1 (destination account, pending block) to (source account, amount). (Removed)
	 * oslo::account, oslo::block_hash -> oslo::account, oslo::amount
	 */
	MDB_dbi pending_v1{ 0 };

	/**
	 * Maps (destination account, pending block) to (source account, amount, version). (Removed)
	 * oslo::account, oslo::block_hash -> oslo::account, oslo::amount, oslo::epoch
	 */
	MDB_dbi pending{ 0 };

	/**
	 * Maps block hash to account and balance. (Removed)
	 * block_hash -> oslo::account, oslo::amount
	 */
	MDB_dbi blocks_info{ 0 };

	/**
	 * Representative weights. (Removed)
	 * oslo::account -> oslo::uint128_t
	 */
	MDB_dbi representation{ 0 };

	/**
	 * Unchecked bootstrap blocks info.
	 * oslo::block_hash -> oslo::unchecked_info
	 */
	MDB_dbi unchecked{ 0 };

	/**
	 * Highest vote observed for account.
	 * oslo::account -> uint64_t
	 */
	MDB_dbi vote{ 0 };

	/**
	 * Samples of online vote weight
	 * uint64_t -> oslo::amount
	 */
	MDB_dbi online_weight{ 0 };

	/**
	 * Meta information about block store, such as versions.
	 * oslo::uint256_union (arbitrary key) -> blob
	 */
	MDB_dbi meta{ 0 };

	/*
	 * Endpoints for peers
	 * oslo::endpoint_key -> no_value
	*/
	MDB_dbi peers{ 0 };

	/*
	 * Confirmation height of an account, and the hash for the block at that height
	 * oslo::account -> uint64_t, oslo::block_hash
	 */
	MDB_dbi confirmation_height{ 0 };

	bool exists (oslo::transaction const & transaction_a, tables table_a, oslo::mdb_val const & key_a) const;

	int get (oslo::transaction const & transaction_a, tables table_a, oslo::mdb_val const & key_a, oslo::mdb_val & value_a) const;
	int put (oslo::write_transaction const & transaction_a, tables table_a, oslo::mdb_val const & key_a, const oslo::mdb_val & value_a) const;
	int del (oslo::write_transaction const & transaction_a, tables table_a, oslo::mdb_val const & key_a) const;

	bool copy_db (boost::filesystem::path const & destination_file) override;
	void rebuild_db (oslo::write_transaction const & transaction_a) override;

	template <typename Key, typename Value>
	oslo::store_iterator<Key, Value> make_iterator (oslo::transaction const & transaction_a, tables table_a) const
	{
		return oslo::store_iterator<Key, Value> (std::make_unique<oslo::mdb_iterator<Key, Value>> (transaction_a, table_to_dbi (table_a)));
	}

	template <typename Key, typename Value>
	oslo::store_iterator<Key, Value> make_iterator (oslo::transaction const & transaction_a, tables table_a, oslo::mdb_val const & key) const
	{
		return oslo::store_iterator<Key, Value> (std::make_unique<oslo::mdb_iterator<Key, Value>> (transaction_a, table_to_dbi (table_a), key));
	}

	bool init_error () const override;

	size_t count (oslo::transaction const &, MDB_dbi) const;

	// These are only use in the upgrade process.
	std::shared_ptr<oslo::block> block_get_v14 (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a, oslo::block_sideband_v14 * sideband_a = nullptr, bool * is_state_v1 = nullptr) const override;
	bool entry_has_sideband_v14 (size_t entry_size_a, oslo::block_type type_a) const;
	size_t block_successor_offset_v14 (oslo::transaction const & transaction_a, size_t entry_size_a, oslo::block_type type_a) const;
	oslo::block_hash block_successor_v14 (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a) const;
	oslo::mdb_val block_raw_get_v14 (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a, oslo::block_type & type_a, bool * is_state_v1 = nullptr) const;
	boost::optional<oslo::mdb_val> block_raw_get_by_type_v14 (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a, oslo::block_type & type_a, bool * is_state_v1) const;
	oslo::account block_account_computed_v14 (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a) const;
	oslo::account block_account_v14 (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a) const;
	oslo::uint128_t block_balance_computed_v14 (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a) const;

private:
	bool do_upgrades (oslo::write_transaction &, bool &, size_t);
	void upgrade_v1_to_v2 (oslo::write_transaction const &);
	void upgrade_v2_to_v3 (oslo::write_transaction const &);
	void upgrade_v3_to_v4 (oslo::write_transaction const &);
	void upgrade_v4_to_v5 (oslo::write_transaction const &);
	void upgrade_v5_to_v6 (oslo::write_transaction const &);
	void upgrade_v6_to_v7 (oslo::write_transaction const &);
	void upgrade_v7_to_v8 (oslo::write_transaction const &);
	void upgrade_v8_to_v9 (oslo::write_transaction const &);
	void upgrade_v10_to_v11 (oslo::write_transaction const &);
	void upgrade_v11_to_v12 (oslo::write_transaction const &);
	void upgrade_v12_to_v13 (oslo::write_transaction &, size_t);
	void upgrade_v13_to_v14 (oslo::write_transaction const &);
	void upgrade_v14_to_v15 (oslo::write_transaction &);
	void upgrade_v15_to_v16 (oslo::write_transaction const &);
	void upgrade_v16_to_v17 (oslo::write_transaction const &);
	void upgrade_v17_to_v18 (oslo::write_transaction const &);

	void open_databases (bool &, oslo::transaction const &, unsigned);

	int drop (oslo::write_transaction const & transaction_a, tables table_a) override;
	int clear (oslo::write_transaction const & transaction_a, MDB_dbi handle_a);

	bool not_found (int status) const override;
	bool success (int status) const override;
	int status_code_not_found () const override;

	MDB_dbi table_to_dbi (tables table_a) const;

	oslo::mdb_txn_tracker mdb_txn_tracker;
	oslo::mdb_txn_callbacks create_txn_callbacks ();
	bool txn_tracking_enabled;

	size_t count (oslo::transaction const & transaction_a, tables table_a) const override;

	bool vacuum_after_upgrade (boost::filesystem::path const & path_a, oslo::lmdb_config const & lmdb_config_a);

	class upgrade_counters
	{
	public:
		upgrade_counters (uint64_t count_before_v0, uint64_t count_before_v1);
		bool are_equal () const;

		uint64_t before_v0;
		uint64_t before_v1;
		uint64_t after_v0{ 0 };
		uint64_t after_v1{ 0 };
	};
};

template <>
void * mdb_val::data () const;
template <>
size_t mdb_val::size () const;
template <>
mdb_val::db_val (size_t size_a, void * data_a);
template <>
void mdb_val::convert_buffer_to_value ();

extern template class block_store_partial<MDB_val, mdb_store>;
}
