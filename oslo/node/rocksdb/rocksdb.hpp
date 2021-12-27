#pragma once

#include <oslo/lib/config.hpp>
#include <oslo/lib/logger_mt.hpp>
#include <oslo/lib/numbers.hpp>
#include <oslo/node/rocksdb/rocksdb_iterator.hpp>
#include <oslo/secure/blockstore_partial.hpp>
#include <oslo/secure/common.hpp>

#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/table.h>
#include <rocksdb/utilities/optimistic_transaction_db.h>
#include <rocksdb/utilities/transaction.h>

namespace oslo
{
class logging_mt;
class rocksdb_config;

/**
 * rocksdb implementation of the block store
 */
class rocksdb_store : public block_store_partial<rocksdb::Slice, rocksdb_store>
{
public:
	rocksdb_store (oslo::logger_mt &, boost::filesystem::path const &, oslo::rocksdb_config const & = oslo::rocksdb_config{}, bool open_read_only = false);
	~rocksdb_store ();
	oslo::write_transaction tx_begin_write (std::vector<oslo::tables> const & tables_requiring_lock = {}, std::vector<oslo::tables> const & tables_no_lock = {}) override;
	oslo::read_transaction tx_begin_read () override;

	std::string vendor_get () const override;

	bool block_info_get (oslo::transaction const &, oslo::block_hash const &, oslo::block_info &) const override;
	size_t count (oslo::transaction const & transaction_a, tables table_a) const override;
	void version_put (oslo::write_transaction const &, int) override;

	bool exists (oslo::transaction const & transaction_a, tables table_a, oslo::rocksdb_val const & key_a) const;
	int get (oslo::transaction const & transaction_a, tables table_a, oslo::rocksdb_val const & key_a, oslo::rocksdb_val & value_a) const;
	int put (oslo::write_transaction const & transaction_a, tables table_a, oslo::rocksdb_val const & key_a, oslo::rocksdb_val const & value_a);
	int del (oslo::write_transaction const & transaction_a, tables table_a, oslo::rocksdb_val const & key_a);

	void serialize_mdb_tracker (boost::property_tree::ptree &, std::chrono::milliseconds, std::chrono::milliseconds) override
	{
		// Do nothing
	}

	std::shared_ptr<oslo::block> block_get_v14 (oslo::transaction const &, oslo::block_hash const &, oslo::block_sideband_v14 * = nullptr, bool * = nullptr) const override
	{
		// Should not be called as RocksDB has no such upgrade path
		release_assert (false);
		return nullptr;
	}

	bool copy_db (boost::filesystem::path const & destination) override;
	void rebuild_db (oslo::write_transaction const & transaction_a) override;

	template <typename Key, typename Value>
	oslo::store_iterator<Key, Value> make_iterator (oslo::transaction const & transaction_a, tables table_a) const
	{
		return oslo::store_iterator<Key, Value> (std::make_unique<oslo::rocksdb_iterator<Key, Value>> (db, transaction_a, table_to_column_family (table_a)));
	}

	template <typename Key, typename Value>
	oslo::store_iterator<Key, Value> make_iterator (oslo::transaction const & transaction_a, tables table_a, oslo::rocksdb_val const & key) const
	{
		return oslo::store_iterator<Key, Value> (std::make_unique<oslo::rocksdb_iterator<Key, Value>> (db, transaction_a, table_to_column_family (table_a), key));
	}

	bool init_error () const override;

private:
	bool error{ false };
	oslo::logger_mt & logger;
	std::vector<rocksdb::ColumnFamilyHandle *> handles;
	// Optimistic transactions are used in write mode
	rocksdb::OptimisticTransactionDB * optimistic_db = nullptr;
	rocksdb::DB * db = nullptr;
	std::shared_ptr<rocksdb::TableFactory> table_factory;
	std::unordered_map<oslo::tables, std::mutex> write_lock_mutexes;

	rocksdb::Transaction * tx (oslo::transaction const & transaction_a) const;
	std::vector<oslo::tables> all_tables () const;

	bool not_found (int status) const override;
	bool success (int status) const override;
	int status_code_not_found () const override;
	int drop (oslo::write_transaction const &, tables) override;

	rocksdb::ColumnFamilyHandle * table_to_column_family (tables table_a) const;
	int clear (rocksdb::ColumnFamilyHandle * column_family);

	void open (bool & error_a, boost::filesystem::path const & path_a, bool open_read_only_a);
	uint64_t count (oslo::transaction const & transaction_a, rocksdb::ColumnFamilyHandle * handle) const;
	bool is_caching_counts (oslo::tables table_a) const;

	int increment (oslo::write_transaction const & transaction_a, tables table_a, oslo::rocksdb_val const & key_a, uint64_t amount_a);
	int decrement (oslo::write_transaction const & transaction_a, tables table_a, oslo::rocksdb_val const & key_a, uint64_t amount_a);
	rocksdb::ColumnFamilyOptions get_cf_options () const;
	void construct_column_family_mutexes ();
	rocksdb::Options get_db_options () const;
	rocksdb::BlockBasedTableOptions get_table_options () const;
	oslo::rocksdb_config rocksdb_config;
};

extern template class block_store_partial<rocksdb::Slice, rocksdb_store>;
}
