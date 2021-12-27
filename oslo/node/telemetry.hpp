#pragma once

#include <oslo/lib/utility.hpp>
#include <oslo/node/common.hpp>
#include <oslo/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>

#include <functional>
#include <memory>

namespace mi = boost::multi_index;

namespace oslo
{
class network;
class alarm;
class worker;
class stat;
namespace transport
{
	class channel;
}

/*
 * Holds a response from a telemetry request
 */
class telemetry_data_response
{
public:
	oslo::telemetry_data telemetry_data;
	oslo::endpoint endpoint;
	bool error{ true };
};

class telemetry_info final
{
public:
	telemetry_info () = default;
	telemetry_info (oslo::endpoint const & endpoint, oslo::telemetry_data const & data, std::chrono::steady_clock::time_point last_response, bool undergoing_request);
	bool awaiting_first_response () const;

	oslo::endpoint endpoint;
	oslo::telemetry_data data;
	std::chrono::steady_clock::time_point last_response;
	bool undergoing_request{ false };
	uint64_t round{ 0 };
};

/*
 * This class requests node telemetry metrics from peers and invokes any callbacks which have been aggregated.
 * All calls to get_metrics return cached data, it does not do any requests, these are periodically done in
 * ongoing_req_all_peers. This can be disabled with the disable_ongoing_telemetry_requests node flag.
 * Calls to get_metrics_single_peer_async will wait until a response is made if it is not within the cache
 * cut off.
 */
class telemetry : public std::enable_shared_from_this<telemetry>
{
public:
	telemetry (oslo::network &, oslo::alarm &, oslo::worker &, oslo::observer_set<oslo::telemetry_data const &, oslo::endpoint const &> &, oslo::stat &, oslo::network_params &, bool);
	void start ();
	void stop ();

	/*
	 * Received telemetry metrics from this peer
	 */
	void set (oslo::telemetry_ack const &, oslo::transport::channel const &);

	/*
	 * This returns what ever is in the cache
	 */
	std::unordered_map<oslo::endpoint, oslo::telemetry_data> get_metrics ();

	/*
	 * This makes a telemetry request to the specific channel.
	 * Error is set for: no response received, no payload received, invalid signature or unsound metrics in message (e.g different genesis block) 
	 */
	void get_metrics_single_peer_async (std::shared_ptr<oslo::transport::channel> const &, std::function<void(telemetry_data_response const &)> const &);

	/*
	 * A blocking version of get_metrics_single_peer_async
	 */
	telemetry_data_response get_metrics_single_peer (std::shared_ptr<oslo::transport::channel> const &);

	/*
	 * Return the number of node metrics collected
	 */
	size_t telemetry_data_size ();

	/*
	 * Returns the time for the cache, response and a small buffer for alarm operations to be scheduled and completed
	 */
	std::chrono::milliseconds cache_plus_buffer_cutoff_time () const;

private:
	class tag_endpoint
	{
	};
	class tag_last_updated
	{
	};

	oslo::network & network;
	oslo::alarm & alarm;
	oslo::worker & worker;
	oslo::observer_set<oslo::telemetry_data const &, oslo::endpoint const &> & observers;
	oslo::stat & stats;
	/* Important that this is a reference to the node network_params for tests which want to modify genesis block */
	oslo::network_params & network_params;
	bool disable_ongoing_requests;

	std::atomic<bool> stopped{ false };

	std::mutex mutex;
	// clang-format off
	// This holds the last telemetry data received from peers or can be a placeholder awaiting the first response (check with awaiting_first_response ())
	boost::multi_index_container<oslo::telemetry_info,
	mi::indexed_by<
		mi::hashed_unique<mi::tag<tag_endpoint>,
			mi::member<oslo::telemetry_info, oslo::endpoint, &oslo::telemetry_info::endpoint>>,
		mi::ordered_non_unique<mi::tag<tag_last_updated>,
			mi::member<oslo::telemetry_info, std::chrono::steady_clock::time_point, &oslo::telemetry_info::last_response>>>> recent_or_initial_request_telemetry_data;
	// clang-format on

	// Anything older than this requires requesting metrics from other nodes.
	std::chrono::seconds const cache_cutoff{ oslo::telemetry_cache_cutoffs::network_to_time (network_params.network) };

	// The maximum time spent waiting for a response to a telemetry request
	std::chrono::seconds const response_time_cutoff{ network_params.network.is_test_network () ? (is_sanitizer_build || oslo::running_within_valgrind () ? 6 : 3) : 10 };

	std::unordered_map<oslo::endpoint, std::vector<std::function<void(telemetry_data_response const &)>>> callbacks;

	void ongoing_req_all_peers (std::chrono::milliseconds);

	void fire_request_message (std::shared_ptr<oslo::transport::channel> const &);
	void channel_processed (oslo::endpoint const &, bool);
	void flush_callbacks_async (oslo::endpoint const &, bool);
	void invoke_callbacks (oslo::endpoint const &, bool);

	bool within_cache_cutoff (oslo::telemetry_info const &) const;
	bool within_cache_plus_buffer_cutoff (telemetry_info const &) const;
	bool verify_message (oslo::telemetry_ack const &, oslo::transport::channel const &);
	friend std::unique_ptr<oslo::container_info_component> collect_container_info (telemetry &, const std::string &);
	friend class telemetry_remove_peer_invalid_signature_Test;
};

std::unique_ptr<oslo::container_info_component> collect_container_info (telemetry & telemetry, const std::string & name);

oslo::telemetry_data consolidate_telemetry_data (std::vector<telemetry_data> const & telemetry_data);
oslo::telemetry_data local_telemetry_data (oslo::ledger_cache const &, oslo::network &, uint64_t, oslo::network_params const &, std::chrono::steady_clock::time_point, uint64_t, oslo::keypair const &);
}
