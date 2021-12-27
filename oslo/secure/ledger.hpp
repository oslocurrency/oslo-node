#pragma once

#include <oslo/lib/rep_weights.hpp>
#include <oslo/secure/common.hpp>

#include <map>

namespace oslo
{
class block_store;
class stat;
class write_transaction;

using tally_t = std::map<oslo::uint128_t, std::shared_ptr<oslo::block>, std::greater<oslo::uint128_t>>;
class ledger final
{
public:
	ledger (oslo::block_store &, oslo::stat &, oslo::generate_cache const & = oslo::generate_cache (), std::function<void()> = nullptr);
	oslo::account account (oslo::transaction const &, oslo::block_hash const &) const;
	oslo::uint128_t amount (oslo::transaction const &, oslo::account const &);
	oslo::uint128_t amount (oslo::transaction const &, oslo::block_hash const &);
	oslo::uint128_t balance (oslo::transaction const &, oslo::block_hash const &) const;
	oslo::uint128_t account_balance (oslo::transaction const &, oslo::account const &);
	oslo::uint128_t account_pending (oslo::transaction const &, oslo::account const &);
	oslo::uint128_t weight (oslo::account const &);
	std::shared_ptr<oslo::block> successor (oslo::transaction const &, oslo::qualified_root const &);
	std::shared_ptr<oslo::block> forked_block (oslo::transaction const &, oslo::block const &);
	std::shared_ptr<oslo::block> backtrack (oslo::transaction const &, std::shared_ptr<oslo::block> const &, uint64_t);
	bool block_confirmed (oslo::transaction const & transaction_a, oslo::block_hash const & hash_a) const;
	bool block_not_confirmed_or_not_exists (oslo::block const & block_a) const;
	oslo::block_hash latest (oslo::transaction const &, oslo::account const &);
	oslo::root latest_root (oslo::transaction const &, oslo::account const &);
	oslo::block_hash representative (oslo::transaction const &, oslo::block_hash const &);
	oslo::block_hash representative_calculated (oslo::transaction const &, oslo::block_hash const &);
	bool block_exists (oslo::block_hash const &);
	bool block_exists (oslo::block_type, oslo::block_hash const &);
	std::string block_text (char const *);
	std::string block_text (oslo::block_hash const &);
	bool is_send (oslo::transaction const &, oslo::state_block const &) const;
	oslo::account const & block_destination (oslo::transaction const &, oslo::block const &);
	oslo::block_hash block_source (oslo::transaction const &, oslo::block const &);
	oslo::process_return process (oslo::write_transaction const &, oslo::block &, oslo::signature_verification = oslo::signature_verification::unknown);
	bool rollback (oslo::write_transaction const &, oslo::block_hash const &, std::vector<std::shared_ptr<oslo::block>> &);
	bool rollback (oslo::write_transaction const &, oslo::block_hash const &);
	void change_latest (oslo::write_transaction const &, oslo::account const &, oslo::account_info const &, oslo::account_info const &);
	void dump_account_chain (oslo::account const &, std::ostream & = std::cout);
	bool could_fit (oslo::transaction const &, oslo::block const &);
	bool can_vote (oslo::transaction const &, oslo::block const &);
	bool is_epoch_link (oslo::link const &);
	std::array<oslo::block_hash, 2> dependent_blocks (oslo::transaction const &, oslo::block const &);
	oslo::account const & epoch_signer (oslo::link const &) const;
	oslo::link const & epoch_link (oslo::epoch) const;
	static oslo::uint128_t const unit;
	oslo::network_params network_params;
	oslo::block_store & store;
	oslo::ledger_cache cache;
	oslo::stat & stats;
	std::unordered_map<oslo::account, oslo::uint128_t> bootstrap_weights;
	std::atomic<size_t> bootstrap_weights_size{ 0 };
	uint64_t bootstrap_weight_max_blocks{ 1 };
	std::atomic<bool> check_bootstrap_weights;
	std::function<void()> epoch_2_started_cb;
};

std::unique_ptr<container_info_component> collect_container_info (ledger & ledger, const std::string & name);
}
