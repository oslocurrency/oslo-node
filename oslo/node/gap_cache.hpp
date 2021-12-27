#pragma once

#include <oslo/lib/numbers.hpp>
#include <oslo/lib/utility.hpp>
#include <oslo/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace oslo
{
class node;
class transaction;

/** For each gap in account chains, track arrival time and voters */
class gap_information final
{
public:
	std::chrono::steady_clock::time_point arrival;
	oslo::block_hash hash;
	std::vector<oslo::account> voters;
	bool bootstrap_started{ false };
};

/** Maintains voting and arrival information for gaps (missing source or previous blocks in account chains) */
class gap_cache final
{
public:
	explicit gap_cache (oslo::node &);
	void add (oslo::block_hash const &, std::chrono::steady_clock::time_point = std::chrono::steady_clock::now ());
	void erase (oslo::block_hash const & hash_a);
	void vote (std::shared_ptr<oslo::vote>);
	bool bootstrap_check (std::vector<oslo::account> const &, oslo::block_hash const &);
	oslo::uint128_t bootstrap_threshold ();
	size_t size ();
	// clang-format off
	class tag_arrival {};
	class tag_hash {};
	using ordered_gaps = boost::multi_index_container<oslo::gap_information,
	boost::multi_index::indexed_by<
		boost::multi_index::ordered_non_unique<boost::multi_index::tag<tag_arrival>,
			boost::multi_index::member<gap_information, std::chrono::steady_clock::time_point, &gap_information::arrival>>,
		boost::multi_index::hashed_unique<boost::multi_index::tag<tag_hash>,
			boost::multi_index::member<gap_information, oslo::block_hash, &gap_information::hash>>>>;
	ordered_gaps blocks;
	// clang-format on
	size_t const max = 256;
	std::mutex mutex;
	oslo::node & node;
};

std::unique_ptr<container_info_component> collect_container_info (gap_cache & gap_cache, const std::string & name);
}
