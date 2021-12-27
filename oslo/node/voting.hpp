#pragma once

#include <oslo/lib/locks.hpp>
#include <oslo/lib/numbers.hpp>
#include <oslo/lib/utility.hpp>
#include <oslo/node/wallet.hpp>
#include <oslo/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace oslo
{
class ledger;
class network;
class node_config;
class vote_processor;
class votes_cache;
class wallets;

class vote_generator final
{
public:
	vote_generator (oslo::node_config const & config_a, oslo::ledger &, oslo::wallets & wallets_a, oslo::vote_processor & vote_processor_a, oslo::votes_cache & votes_cache_a, oslo::network & network_a);
	void add (oslo::block_hash const &);
	void stop ();

private:
	void run ();
	void send (oslo::unique_lock<std::mutex> &);
	oslo::node_config const & config;
	oslo::ledger & ledger;
	oslo::wallets & wallets;
	oslo::vote_processor & vote_processor;
	oslo::votes_cache & votes_cache;
	oslo::network & network;
	std::mutex mutex;
	oslo::condition_variable condition;
	std::deque<oslo::block_hash> hashes;
	oslo::network_params network_params;
	bool stopped{ false };
	bool started{ false };
	std::thread thread;

	friend std::unique_ptr<container_info_component> collect_container_info (vote_generator & vote_generator, const std::string & name);
};

class vote_generator_session final
{
public:
	vote_generator_session (vote_generator & vote_generator_a);
	void add (oslo::block_hash const &);
	void flush ();

private:
	oslo::vote_generator & generator;
	std::vector<oslo::block_hash> hashes;
};

std::unique_ptr<container_info_component> collect_container_info (vote_generator & vote_generator, const std::string & name);
class cached_votes final
{
public:
	oslo::block_hash hash;
	std::vector<std::shared_ptr<oslo::vote>> votes;
};
class votes_cache final
{
public:
	votes_cache (oslo::wallets & wallets_a);
	void add (std::shared_ptr<oslo::vote> const &);
	std::vector<std::shared_ptr<oslo::vote>> find (oslo::block_hash const &);
	void remove (oslo::block_hash const &);

private:
	std::mutex cache_mutex;
	// clang-format off
	class tag_sequence {};
	class tag_hash {};
	boost::multi_index_container<oslo::cached_votes,
	boost::multi_index::indexed_by<
		boost::multi_index::sequenced<boost::multi_index::tag<tag_sequence>>,
		boost::multi_index::hashed_unique<boost::multi_index::tag<tag_hash>,
			boost::multi_index::member<oslo::cached_votes, oslo::block_hash, &oslo::cached_votes::hash>>>>
	cache;
	// clang-format on
	oslo::network_params network_params;
	oslo::wallets & wallets;
	friend std::unique_ptr<container_info_component> collect_container_info (votes_cache & votes_cache, const std::string & name);
};

std::unique_ptr<container_info_component> collect_container_info (votes_cache & votes_cache, const std::string & name);
}
