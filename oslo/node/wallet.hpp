#pragma once

#include <oslo/lib/lmdbconfig.hpp>
#include <oslo/lib/locks.hpp>
#include <oslo/lib/work.hpp>
#include <oslo/node/lmdb/lmdb.hpp>
#include <oslo/node/lmdb/wallet_value.hpp>
#include <oslo/node/openclwork.hpp>
#include <oslo/secure/blockstore.hpp>
#include <oslo/secure/common.hpp>

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_set>
namespace oslo
{
class node;
class node_config;
class wallets;
// The fan spreads a key out over the heap to decrease the likelihood of it being recovered by memory inspection
class fan final
{
public:
	fan (oslo::uint256_union const &, size_t);
	void value (oslo::raw_key &);
	void value_set (oslo::raw_key const &);
	std::vector<std::unique_ptr<oslo::uint256_union>> values;

private:
	std::mutex mutex;
	void value_get (oslo::raw_key &);
};
class kdf final
{
public:
	void phs (oslo::raw_key &, std::string const &, oslo::uint256_union const &);
	std::mutex mutex;
};
enum class key_type
{
	not_a_type,
	unknown,
	adhoc,
	deterministic
};
class wallet_store final
{
public:
	wallet_store (bool &, oslo::kdf &, oslo::transaction &, oslo::account, unsigned, std::string const &);
	wallet_store (bool &, oslo::kdf &, oslo::transaction &, oslo::account, unsigned, std::string const &, std::string const &);
	std::vector<oslo::account> accounts (oslo::transaction const &);
	void initialize (oslo::transaction const &, bool &, std::string const &);
	oslo::uint256_union check (oslo::transaction const &);
	bool rekey (oslo::transaction const &, std::string const &);
	bool valid_password (oslo::transaction const &);
	bool valid_public_key (oslo::public_key const &);
	bool attempt_password (oslo::transaction const &, std::string const &);
	void wallet_key (oslo::raw_key &, oslo::transaction const &);
	void seed (oslo::raw_key &, oslo::transaction const &);
	void seed_set (oslo::transaction const &, oslo::raw_key const &);
	oslo::key_type key_type (oslo::wallet_value const &);
	oslo::public_key deterministic_insert (oslo::transaction const &);
	oslo::public_key deterministic_insert (oslo::transaction const &, uint32_t const);
	oslo::private_key deterministic_key (oslo::transaction const &, uint32_t);
	uint32_t deterministic_index_get (oslo::transaction const &);
	void deterministic_index_set (oslo::transaction const &, uint32_t);
	void deterministic_clear (oslo::transaction const &);
	oslo::uint256_union salt (oslo::transaction const &);
	bool is_representative (oslo::transaction const &);
	oslo::account representative (oslo::transaction const &);
	void representative_set (oslo::transaction const &, oslo::account const &);
	oslo::public_key insert_adhoc (oslo::transaction const &, oslo::raw_key const &);
	bool insert_watch (oslo::transaction const &, oslo::account const &);
	void erase (oslo::transaction const &, oslo::account const &);
	oslo::wallet_value entry_get_raw (oslo::transaction const &, oslo::account const &);
	void entry_put_raw (oslo::transaction const &, oslo::account const &, oslo::wallet_value const &);
	bool fetch (oslo::transaction const &, oslo::account const &, oslo::raw_key &);
	bool exists (oslo::transaction const &, oslo::account const &);
	void destroy (oslo::transaction const &);
	oslo::store_iterator<oslo::account, oslo::wallet_value> find (oslo::transaction const &, oslo::account const &);
	oslo::store_iterator<oslo::account, oslo::wallet_value> begin (oslo::transaction const &, oslo::account const &);
	oslo::store_iterator<oslo::account, oslo::wallet_value> begin (oslo::transaction const &);
	oslo::store_iterator<oslo::account, oslo::wallet_value> end ();
	void derive_key (oslo::raw_key &, oslo::transaction const &, std::string const &);
	void serialize_json (oslo::transaction const &, std::string &);
	void write_backup (oslo::transaction const &, boost::filesystem::path const &);
	bool move (oslo::transaction const &, oslo::wallet_store &, std::vector<oslo::public_key> const &);
	bool import (oslo::transaction const &, oslo::wallet_store &);
	bool work_get (oslo::transaction const &, oslo::public_key const &, uint64_t &);
	void work_put (oslo::transaction const &, oslo::public_key const &, uint64_t);
	unsigned version (oslo::transaction const &);
	void version_put (oslo::transaction const &, unsigned);
	void upgrade_v1_v2 (oslo::transaction const &);
	void upgrade_v2_v3 (oslo::transaction const &);
	void upgrade_v3_v4 (oslo::transaction const &);
	oslo::fan password;
	oslo::fan wallet_key_mem;
	static unsigned const version_1 = 1;
	static unsigned const version_2 = 2;
	static unsigned const version_3 = 3;
	static unsigned const version_4 = 4;
	static unsigned constexpr version_current = version_4;
	static oslo::account const version_special;
	static oslo::account const wallet_key_special;
	static oslo::account const salt_special;
	static oslo::account const check_special;
	static oslo::account const representative_special;
	static oslo::account const seed_special;
	static oslo::account const deterministic_index_special;
	static size_t const check_iv_index;
	static size_t const seed_iv_index;
	static int const special_count;
	oslo::kdf & kdf;
	std::atomic<MDB_dbi> handle{ 0 };
	std::recursive_mutex mutex;

private:
	MDB_txn * tx (oslo::transaction const &) const;
};
// A wallet is a set of account keys encrypted by a common encryption key
class wallet final : public std::enable_shared_from_this<oslo::wallet>
{
public:
	std::shared_ptr<oslo::block> change_action (oslo::account const &, oslo::account const &, uint64_t = 0, bool = true);
	std::shared_ptr<oslo::block> receive_action (oslo::block const &, oslo::account const &, oslo::uint128_union const &, uint64_t = 0, bool = true);
	std::shared_ptr<oslo::block> send_action (oslo::account const &, oslo::account const &, oslo::uint128_t const &, uint64_t = 0, bool = true, boost::optional<std::string> = {});
	bool action_complete (std::shared_ptr<oslo::block> const &, oslo::account const &, bool const, oslo::block_details const &);
	wallet (bool &, oslo::transaction &, oslo::wallets &, std::string const &);
	wallet (bool &, oslo::transaction &, oslo::wallets &, std::string const &, std::string const &);
	void enter_initial_password ();
	bool enter_password (oslo::transaction const &, std::string const &);
	oslo::public_key insert_adhoc (oslo::raw_key const &, bool = true);
	oslo::public_key insert_adhoc (oslo::transaction const &, oslo::raw_key const &, bool = true);
	bool insert_watch (oslo::transaction const &, oslo::public_key const &);
	oslo::public_key deterministic_insert (oslo::transaction const &, bool = true);
	oslo::public_key deterministic_insert (uint32_t, bool = true);
	oslo::public_key deterministic_insert (bool = true);
	bool exists (oslo::public_key const &);
	bool import (std::string const &, std::string const &);
	void serialize (std::string &);
	bool change_sync (oslo::account const &, oslo::account const &);
	void change_async (oslo::account const &, oslo::account const &, std::function<void(std::shared_ptr<oslo::block>)> const &, uint64_t = 0, bool = true);
	bool receive_sync (std::shared_ptr<oslo::block>, oslo::account const &, oslo::uint128_t const &);
	void receive_async (std::shared_ptr<oslo::block>, oslo::account const &, oslo::uint128_t const &, std::function<void(std::shared_ptr<oslo::block>)> const &, uint64_t = 0, bool = true);
	oslo::block_hash send_sync (oslo::account const &, oslo::account const &, oslo::uint128_t const &);
	void send_async (oslo::account const &, oslo::account const &, oslo::uint128_t const &, std::function<void(std::shared_ptr<oslo::block>)> const &, uint64_t = 0, bool = true, boost::optional<std::string> = {});
	void work_cache_blocking (oslo::account const &, oslo::root const &);
	void work_update (oslo::transaction const &, oslo::account const &, oslo::root const &, uint64_t);
	// Schedule work generation after a few seconds
	void work_ensure (oslo::account const &, oslo::root const &);
	bool search_pending ();
	void init_free_accounts (oslo::transaction const &);
	uint32_t deterministic_check (oslo::transaction const & transaction_a, uint32_t index);
	/** Changes the wallet seed and returns the first account */
	oslo::public_key change_seed (oslo::transaction const & transaction_a, oslo::raw_key const & prv_a, uint32_t count = 0);
	void deterministic_restore (oslo::transaction const & transaction_a);
	bool live ();
	oslo::network_params network_params;
	std::unordered_set<oslo::account> free_accounts;
	std::function<void(bool, bool)> lock_observer;
	oslo::wallet_store store;
	oslo::wallets & wallets;
	std::mutex representatives_mutex;
	std::unordered_set<oslo::account> representatives;
};

class work_watcher final : public std::enable_shared_from_this<oslo::work_watcher>
{
public:
	work_watcher (oslo::node &);
	~work_watcher ();
	void stop ();
	void add (std::shared_ptr<oslo::block>);
	void update (oslo::qualified_root const &, std::shared_ptr<oslo::state_block>);
	void watching (oslo::qualified_root const &, std::shared_ptr<oslo::state_block>);
	void remove (oslo::block const &);
	bool is_watched (oslo::qualified_root const &);
	size_t size ();
	std::mutex mutex;
	oslo::node & node;
	std::unordered_map<oslo::qualified_root, std::shared_ptr<oslo::state_block>> watched;
	std::atomic<bool> stopped;
};

class wallet_representatives
{
public:
	uint64_t voting{ 0 }; // Number of representatives with at least the configured minimum voting weight
	uint64_t half_principal{ 0 }; // Number of representatives with at least 50% of principal representative requirements
	std::unordered_set<oslo::account> accounts; // Representatives with at least the configured minimum voting weight
	bool have_half_rep () const
	{
		return half_principal > 0;
	}
	bool exists (oslo::account const & rep_a) const
	{
		return accounts.count (rep_a) > 0;
	}
	void clear ()
	{
		voting = 0;
		half_principal = 0;
		accounts.clear ();
	}
};

/**
 * The wallets set is all the wallets a node controls.
 * A node may contain multiple wallets independently encrypted and operated.
 */
class wallets final
{
public:
	wallets (bool, oslo::node &);
	~wallets ();
	std::shared_ptr<oslo::wallet> open (oslo::wallet_id const &);
	std::shared_ptr<oslo::wallet> create (oslo::wallet_id const &);
	bool search_pending (oslo::wallet_id const &);
	void search_pending_all ();
	void destroy (oslo::wallet_id const &);
	void reload ();
	void do_wallet_actions ();
	void queue_wallet_action (oslo::uint128_t const &, std::shared_ptr<oslo::wallet>, std::function<void(oslo::wallet &)> const &);
	void foreach_representative (std::function<void(oslo::public_key const &, oslo::raw_key const &)> const &);
	bool exists (oslo::transaction const &, oslo::account const &);
	void stop ();
	void clear_send_ids (oslo::transaction const &);
	oslo::wallet_representatives reps () const;
	bool check_rep (oslo::account const &, oslo::uint128_t const &, const bool = true);
	void compute_reps ();
	void ongoing_compute_reps ();
	void split_if_needed (oslo::transaction &, oslo::block_store &);
	void move_table (std::string const &, MDB_txn *, MDB_txn *);
	oslo::network_params network_params;
	std::function<void(bool)> observer;
	std::unordered_map<oslo::wallet_id, std::shared_ptr<oslo::wallet>> items;
	std::multimap<oslo::uint128_t, std::pair<std::shared_ptr<oslo::wallet>, std::function<void(oslo::wallet &)>>, std::greater<oslo::uint128_t>> actions;
	oslo::locked<std::unordered_map<oslo::account, oslo::root>> delayed_work;
	std::mutex mutex;
	std::mutex action_mutex;
	oslo::condition_variable condition;
	oslo::kdf kdf;
	MDB_dbi handle;
	MDB_dbi send_action_ids;
	oslo::node & node;
	oslo::mdb_env & env;
	std::atomic<bool> stopped;
	std::shared_ptr<oslo::work_watcher> watcher;
	std::thread thread;
	static oslo::uint128_t const generate_priority;
	static oslo::uint128_t const high_priority;
	/** Start read-write transaction */
	oslo::write_transaction tx_begin_write ();

	/** Start read-only transaction */
	oslo::read_transaction tx_begin_read ();

private:
	mutable std::mutex reps_cache_mutex;
	oslo::wallet_representatives representatives;
};

std::unique_ptr<container_info_component> collect_container_info (wallets & wallets, const std::string & name);

class wallets_store
{
public:
	virtual ~wallets_store () = default;
	virtual bool init_error () const = 0;
};
class mdb_wallets_store final : public wallets_store
{
public:
	mdb_wallets_store (boost::filesystem::path const &, oslo::lmdb_config const & lmdb_config_a = oslo::lmdb_config{});
	oslo::mdb_env environment;
	bool init_error () const override;
	bool error{ false };
};
}
