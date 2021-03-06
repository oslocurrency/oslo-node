#include <oslo/crypto_lib/random_pool.hpp>
#include <oslo/node/bootstrap/bootstrap.hpp>
#include <oslo/node/bootstrap/bootstrap_attempt.hpp>
#include <oslo/node/bootstrap/bootstrap_bulk_push.hpp>
#include <oslo/node/bootstrap/bootstrap_frontier.hpp>
#include <oslo/node/common.hpp>
#include <oslo/node/node.hpp>
#include <oslo/node/transport/tcp.hpp>
#include <oslo/node/websocket.hpp>

#include <boost/format.hpp>

#include <algorithm>

constexpr size_t oslo::bootstrap_limits::bootstrap_max_confirm_frontiers;
constexpr double oslo::bootstrap_limits::required_frontier_confirmation_ratio;
constexpr unsigned oslo::bootstrap_limits::frontier_confirmation_blocks_limit;
constexpr unsigned oslo::bootstrap_limits::requeued_pulls_limit;
constexpr unsigned oslo::bootstrap_limits::requeued_pulls_limit_test;

oslo::bootstrap_attempt::bootstrap_attempt (std::shared_ptr<oslo::node> node_a, oslo::bootstrap_mode mode_a, uint64_t incremental_id_a, std::string id_a) :
node (node_a),
incremental_id (incremental_id_a),
id (id_a),
mode (mode_a)
{
	if (id.empty ())
	{
		oslo::random_constants constants;
		id = constants.random_128.to_string ();
	}
	node->logger.always_log (boost::str (boost::format ("Starting %1% bootstrap attempt with ID %2%") % mode_text () % id));
	node->bootstrap_initiator.notify_listeners (true);
	if (node->websocket_server)
	{
		oslo::websocket::message_builder builder;
		node->websocket_server->broadcast (builder.bootstrap_started (id, mode_text ()));
	}
}

oslo::bootstrap_attempt::~bootstrap_attempt ()
{
	node->logger.always_log (boost::str (boost::format ("Exiting %1% bootstrap attempt with ID %2%") % mode_text () % id));
	node->bootstrap_initiator.notify_listeners (false);
	if (node->websocket_server)
	{
		oslo::websocket::message_builder builder;
		node->websocket_server->broadcast (builder.bootstrap_exited (id, mode_text (), attempt_start, total_blocks));
	}
}

bool oslo::bootstrap_attempt::should_log ()
{
	oslo::lock_guard<std::mutex> guard (next_log_mutex);
	auto result (false);
	auto now (std::chrono::steady_clock::now ());
	if (next_log < now)
	{
		result = true;
		next_log = now + std::chrono::seconds (15);
	}
	return result;
}

bool oslo::bootstrap_attempt::still_pulling ()
{
	debug_assert (!mutex.try_lock ());
	auto running (!stopped);
	auto still_pulling (pulling > 0);
	return running && still_pulling;
}

void oslo::bootstrap_attempt::pull_started ()
{
	{
		oslo::lock_guard<std::mutex> guard (mutex);
		++pulling;
	}
	condition.notify_all ();
}

void oslo::bootstrap_attempt::pull_finished ()
{
	{
		oslo::lock_guard<std::mutex> guard (mutex);
		--pulling;
	}
	condition.notify_all ();
}

void oslo::bootstrap_attempt::stop ()
{
	{
		oslo::lock_guard<std::mutex> lock (mutex);
		stopped = true;
	}
	condition.notify_all ();
	node->bootstrap_initiator.connections->clear_pulls (incremental_id);
}

std::string oslo::bootstrap_attempt::mode_text ()
{
	std::string mode_text;
	if (mode == oslo::bootstrap_mode::legacy)
	{
		mode_text = "legacy";
	}
	else if (mode == oslo::bootstrap_mode::lazy)
	{
		mode_text = "lazy";
	}
	else if (mode == oslo::bootstrap_mode::wallet_lazy)
	{
		mode_text = "wallet_lazy";
	}
	return mode_text;
}

void oslo::bootstrap_attempt::restart_condition ()
{
	debug_assert (mode == oslo::bootstrap_mode::legacy);
}

void oslo::bootstrap_attempt::add_frontier (oslo::pull_info const &)
{
	debug_assert (mode == oslo::bootstrap_mode::legacy);
}

void oslo::bootstrap_attempt::add_bulk_push_target (oslo::block_hash const &, oslo::block_hash const &)
{
	debug_assert (mode == oslo::bootstrap_mode::legacy);
}

bool oslo::bootstrap_attempt::request_bulk_push_target (std::pair<oslo::block_hash, oslo::block_hash> &)
{
	debug_assert (mode == oslo::bootstrap_mode::legacy);
	return true;
}

void oslo::bootstrap_attempt::add_recent_pull (oslo::block_hash const &)
{
	debug_assert (mode == oslo::bootstrap_mode::legacy);
}

bool oslo::bootstrap_attempt::process_block (std::shared_ptr<oslo::block> block_a, oslo::account const & known_account_a, uint64_t pull_blocks, oslo::bulk_pull::count_t max_blocks, bool block_expected, unsigned retry_limit)
{
	oslo::unchecked_info info (block_a, known_account_a, 0, oslo::signature_verification::unknown);
	node->block_processor.add (info);
	return false;
}

void oslo::bootstrap_attempt::lazy_start (oslo::hash_or_account const &, bool)
{
	debug_assert (mode == oslo::bootstrap_mode::lazy);
}

void oslo::bootstrap_attempt::lazy_add (oslo::pull_info const &)
{
	debug_assert (mode == oslo::bootstrap_mode::lazy);
}

void oslo::bootstrap_attempt::lazy_requeue (oslo::block_hash const &, oslo::block_hash const &, bool)
{
	debug_assert (mode == oslo::bootstrap_mode::lazy);
}

uint32_t oslo::bootstrap_attempt::lazy_batch_size ()
{
	debug_assert (mode == oslo::bootstrap_mode::lazy);
	return node->network_params.bootstrap.lazy_min_pull_blocks;
}

bool oslo::bootstrap_attempt::lazy_processed_or_exists (oslo::block_hash const &)
{
	debug_assert (mode == oslo::bootstrap_mode::lazy);
	return false;
}

bool oslo::bootstrap_attempt::lazy_has_expired () const
{
	debug_assert (mode == oslo::bootstrap_mode::lazy);
	return true;
}

void oslo::bootstrap_attempt::requeue_pending (oslo::account const &)
{
	debug_assert (mode == oslo::bootstrap_mode::wallet_lazy);
}

void oslo::bootstrap_attempt::wallet_start (std::deque<oslo::account> &)
{
	debug_assert (mode == oslo::bootstrap_mode::wallet_lazy);
}

size_t oslo::bootstrap_attempt::wallet_size ()
{
	debug_assert (mode == oslo::bootstrap_mode::wallet_lazy);
	return 0;
}

oslo::bootstrap_attempt_legacy::bootstrap_attempt_legacy (std::shared_ptr<oslo::node> node_a, uint64_t incremental_id_a, std::string id_a) :
oslo::bootstrap_attempt (node_a, oslo::bootstrap_mode::legacy, incremental_id_a, id_a)
{
	node->bootstrap_initiator.notify_listeners (true);
}

bool oslo::bootstrap_attempt_legacy::consume_future (std::future<bool> & future_a)
{
	bool result;
	try
	{
		result = future_a.get ();
	}
	catch (std::future_error &)
	{
		result = true;
	}
	return result;
}

void oslo::bootstrap_attempt_legacy::stop ()
{
	oslo::unique_lock<std::mutex> lock (mutex);
	stopped = true;
	lock.unlock ();
	condition.notify_all ();
	lock.lock ();
	if (auto i = frontiers.lock ())
	{
		try
		{
			i->promise.set_value (true);
		}
		catch (std::future_error &)
		{
		}
	}
	if (auto i = push.lock ())
	{
		try
		{
			i->promise.set_value (true);
		}
		catch (std::future_error &)
		{
		}
	}
	lock.unlock ();
	node->bootstrap_initiator.connections->clear_pulls (incremental_id);
}

void oslo::bootstrap_attempt_legacy::request_push (oslo::unique_lock<std::mutex> & lock_a)
{
	bool error (false);
	lock_a.unlock ();
	auto connection_l (node->bootstrap_initiator.connections->find_connection (endpoint_frontier_request));
	lock_a.lock ();
	if (connection_l)
	{
		std::future<bool> future;
		{
			auto this_l (shared_from_this ());
			auto client (std::make_shared<oslo::bulk_push_client> (connection_l, this_l));
			client->start ();
			push = client;
			future = client->promise.get_future ();
		}
		lock_a.unlock ();
		error = consume_future (future); // This is out of scope of `client' so when the last reference via boost::asio::io_context is lost and the client is destroyed, the future throws an exception.
		lock_a.lock ();
	}
	if (node->config.logging.network_logging ())
	{
		node->logger.try_log ("Exiting bulk push client");
		if (error)
		{
			node->logger.try_log ("Bulk push client failed");
		}
	}
}

void oslo::bootstrap_attempt_legacy::add_frontier (oslo::pull_info const & pull_a)
{
	oslo::pull_info pull (pull_a);
	oslo::lock_guard<std::mutex> lock (mutex);
	frontier_pulls.push_back (pull);
}

void oslo::bootstrap_attempt_legacy::add_bulk_push_target (oslo::block_hash const & head, oslo::block_hash const & end)
{
	oslo::lock_guard<std::mutex> lock (mutex);
	bulk_push_targets.emplace_back (head, end);
}

bool oslo::bootstrap_attempt_legacy::request_bulk_push_target (std::pair<oslo::block_hash, oslo::block_hash> & current_target_a)
{
	oslo::lock_guard<std::mutex> lock (mutex);
	auto empty (bulk_push_targets.empty ());
	if (!empty)
	{
		current_target_a = bulk_push_targets.back ();
		bulk_push_targets.pop_back ();
	}
	return empty;
}

void oslo::bootstrap_attempt_legacy::add_recent_pull (oslo::block_hash const & head_a)
{
	oslo::lock_guard<std::mutex> lock (mutex);
	recent_pulls_head.push_back (head_a);
	if (recent_pulls_head.size () > oslo::bootstrap_limits::bootstrap_max_confirm_frontiers)
	{
		recent_pulls_head.pop_front ();
	}
}

void oslo::bootstrap_attempt_legacy::restart_condition ()
{
	/* Conditions to start frontiers confirmation:
	- not completed frontiers confirmation
	- more than 256 pull retries usually indicating issues with requested pulls
	- or 128k processed blocks indicating large bootstrap */
	if (!frontiers_confirmation_pending && !frontiers_confirmed && (requeued_pulls > (!node->network_params.network.is_test_network () ? oslo::bootstrap_limits::requeued_pulls_limit : oslo::bootstrap_limits::requeued_pulls_limit_test) || total_blocks > oslo::bootstrap_limits::frontier_confirmation_blocks_limit))
	{
		frontiers_confirmation_pending = true;
	}
}

void oslo::bootstrap_attempt_legacy::attempt_restart_check (oslo::unique_lock<std::mutex> & lock_a)
{
	if (frontiers_confirmation_pending)
	{
		auto confirmed (confirm_frontiers (lock_a));
		debug_assert (lock_a.owns_lock ());
		if (!confirmed)
		{
			node->stats.inc (oslo::stat::type::bootstrap, oslo::stat::detail::frontier_confirmation_failed, oslo::stat::dir::in);
			auto score (node->network.excluded_peers.add (endpoint_frontier_request, node->network.size ()));
			if (score >= oslo::peer_exclusion::score_limit)
			{
				node->logger.always_log (boost::str (boost::format ("Adding peer %1% to excluded peers list with score %2% after %3% seconds bootstrap attempt") % endpoint_frontier_request % score % std::chrono::duration_cast<std::chrono::seconds> (std::chrono::steady_clock::now () - attempt_start).count ()));
				auto channel = node->network.find_channel (oslo::transport::map_tcp_to_endpoint (endpoint_frontier_request));
				if (channel != nullptr)
				{
					node->network.erase (*channel);
				}
			}
			lock_a.unlock ();
			stop ();
			lock_a.lock ();
			// Start new bootstrap connection
			auto node_l (node->shared ());
			node->background ([node_l]() {
				node_l->bootstrap_initiator.bootstrap (true);
			});
		}
		else
		{
			node->stats.inc (oslo::stat::type::bootstrap, oslo::stat::detail::frontier_confirmation_successful, oslo::stat::dir::in);
		}
		frontiers_confirmed = confirmed;
		frontiers_confirmation_pending = false;
	}
}

bool oslo::bootstrap_attempt_legacy::confirm_frontiers (oslo::unique_lock<std::mutex> & lock_a)
{
	bool confirmed (false);
	debug_assert (!frontiers_confirmed);
	condition.wait (lock_a, [& stopped = stopped] { return !stopped; });
	auto this_l (shared_from_this ());
	std::vector<oslo::block_hash> frontiers;
	lock_a.unlock ();
	oslo::unique_lock<std::mutex> pulls_lock (node->bootstrap_initiator.connections->mutex);
	for (auto i (node->bootstrap_initiator.connections->pulls.begin ()), end (node->bootstrap_initiator.connections->pulls.end ()); i != end && frontiers.size () != oslo::bootstrap_limits::bootstrap_max_confirm_frontiers; ++i)
	{
		if (!i->head.is_zero () && i->bootstrap_id == incremental_id && std::find (frontiers.begin (), frontiers.end (), i->head) == frontiers.end ())
		{
			frontiers.push_back (i->head);
		}
	}
	pulls_lock.unlock ();
	lock_a.lock ();
	for (auto i (recent_pulls_head.begin ()), end (recent_pulls_head.end ()); i != end && frontiers.size () != oslo::bootstrap_limits::bootstrap_max_confirm_frontiers; ++i)
	{
		if (!i->is_zero () && std::find (frontiers.begin (), frontiers.end (), *i) == frontiers.end ())
		{
			frontiers.push_back (*i);
		}
	}
	lock_a.unlock ();
	auto frontiers_count (frontiers.size ());
	if (frontiers_count > 0)
	{
		const size_t reps_limit = 20;
		auto representatives (node->rep_crawler.representatives ());
		auto reps_weight (node->rep_crawler.total_weight ());
		auto representatives_copy (representatives);
		oslo::uint128_t total_weight (0);
		// Select random peers from bottom 50% of principal representatives
		if (representatives.size () > 1)
		{
			std::reverse (representatives.begin (), representatives.end ());
			representatives.resize (representatives.size () / 2);
			for (auto i = static_cast<CryptoPP::word32> (representatives.size () - 1); i > 0; --i)
			{
				auto k = oslo::random_pool::generate_word32 (0, i);
				std::swap (representatives[i], representatives[k]);
			}
			if (representatives.size () > reps_limit)
			{
				representatives.resize (reps_limit);
			}
		}
		for (auto const & rep : representatives)
		{
			total_weight += rep.weight.number ();
		}
		// Select peers with total 25% of reps stake from top 50% of principal representatives
		representatives_copy.resize (representatives_copy.size () / 2);
		while (total_weight < reps_weight / 4) // 25%
		{
			auto k = oslo::random_pool::generate_word32 (0, static_cast<CryptoPP::word32> (representatives_copy.size () - 1));
			auto rep (representatives_copy[k]);
			if (std::find (representatives.begin (), representatives.end (), rep) == representatives.end ())
			{
				representatives.push_back (rep);
				total_weight += rep.weight.number ();
			}
		}
		// Start requests
		for (auto i (0), max_requests (20); i <= max_requests && !confirmed && !stopped; ++i)
		{
			std::unordered_map<std::shared_ptr<oslo::transport::channel>, std::deque<std::pair<oslo::block_hash, oslo::root>>> batched_confirm_req_bundle;
			std::deque<std::pair<oslo::block_hash, oslo::root>> request;
			// Find confirmed frontiers (tally > 12.5% of reps stake, 60% of requestsed reps responded
			for (auto ii (frontiers.begin ()); ii != frontiers.end ();)
			{
				if (node->ledger.block_exists (*ii))
				{
					ii = frontiers.erase (ii);
				}
				else
				{
					oslo::unique_lock<std::mutex> active_lock (node->active.mutex);
					auto existing (node->active.find_inactive_votes_cache (*ii));
					active_lock.unlock ();
					oslo::uint128_t tally;
					for (auto & voter : existing.voters)
					{
						tally += node->ledger.weight (voter);
					}
					if (existing.confirmed || (tally > reps_weight / 8 && existing.voters.size () >= representatives.size () * 0.6)) // 12.5% of weight, 60% of reps
					{
						ii = frontiers.erase (ii);
					}
					else
					{
						for (auto const & rep : representatives)
						{
							if (std::find (existing.voters.begin (), existing.voters.end (), rep.account) == existing.voters.end ())
							{
								release_assert (!ii->is_zero ());
								auto rep_request (batched_confirm_req_bundle.find (rep.channel));
								if (rep_request == batched_confirm_req_bundle.end ())
								{
									std::deque<std::pair<oslo::block_hash, oslo::root>> insert_root_hash = { std::make_pair (*ii, *ii) };
									batched_confirm_req_bundle.emplace (rep.channel, insert_root_hash);
								}
								else
								{
									rep_request->second.emplace_back (*ii, *ii);
								}
							}
						}
						++ii;
					}
				}
			}
			auto confirmed_count (frontiers_count - frontiers.size ());
			if (confirmed_count >= frontiers_count * oslo::bootstrap_limits::required_frontier_confirmation_ratio) // 80% of frontiers confirmed
			{
				confirmed = true;
			}
			else if (i < max_requests)
			{
				node->network.broadcast_confirm_req_batched_many (batched_confirm_req_bundle);
				std::this_thread::sleep_for (std::chrono::milliseconds (!node->network_params.network.is_test_network () ? 500 : 5));
			}
		}
		if (!confirmed)
		{
			node->logger.always_log (boost::str (boost::format ("Failed to confirm frontiers for bootstrap attempt. %1% of %2% frontiers were not confirmed") % frontiers.size () % frontiers_count));
		}
	}
	lock_a.lock ();
	return confirmed;
}

bool oslo::bootstrap_attempt_legacy::request_frontier (oslo::unique_lock<std::mutex> & lock_a, bool first_attempt)
{
	auto result (true);
	lock_a.unlock ();
	auto connection_l (node->bootstrap_initiator.connections->connection (shared_from_this (), first_attempt));
	lock_a.lock ();
	if (connection_l && !stopped)
	{
		endpoint_frontier_request = connection_l->channel->get_tcp_endpoint ();
		std::future<bool> future;
		{
			auto this_l (shared_from_this ());
			auto client (std::make_shared<oslo::frontier_req_client> (connection_l, this_l));
			client->run ();
			frontiers = client;
			future = client->promise.get_future ();
		}
		lock_a.unlock ();
		result = consume_future (future); // This is out of scope of `client' so when the last reference via boost::asio::io_context is lost and the client is destroyed, the future throws an exception.
		lock_a.lock ();
		if (result)
		{
			frontier_pulls.clear ();
		}
		else
		{
			account_count = frontier_pulls.size ();
			// Shuffle pulls
			release_assert (std::numeric_limits<CryptoPP::word32>::max () > frontier_pulls.size ());
			if (!frontier_pulls.empty ())
			{
				for (auto i = static_cast<CryptoPP::word32> (frontier_pulls.size () - 1); i > 0; --i)
				{
					auto k = oslo::random_pool::generate_word32 (0, i);
					std::swap (frontier_pulls[i], frontier_pulls[k]);
				}
			}
			// Add to regular pulls
			while (!frontier_pulls.empty ())
			{
				auto pull (frontier_pulls.front ());
				lock_a.unlock ();
				node->bootstrap_initiator.connections->add_pull (pull);
				lock_a.lock ();
				++pulling;
				frontier_pulls.pop_front ();
			}
		}
		if (node->config.logging.network_logging ())
		{
			if (!result)
			{
				node->logger.try_log (boost::str (boost::format ("Completed frontier request, %1% out of sync accounts according to %2%") % account_count % connection_l->channel->to_string ()));
			}
			else
			{
				node->stats.inc (oslo::stat::type::error, oslo::stat::detail::frontier_req, oslo::stat::dir::out);
			}
		}
	}
	return result;
}

void oslo::bootstrap_attempt_legacy::run_start (oslo::unique_lock<std::mutex> & lock_a)
{
	frontiers_received = false;
	frontiers_confirmed = false;
	total_blocks = 0;
	requeued_pulls = 0;
	recent_pulls_head.clear ();
	auto frontier_failure (true);
	uint64_t frontier_attempts (0);
	while (!stopped && frontier_failure)
	{
		++frontier_attempts;
		frontier_failure = request_frontier (lock_a, frontier_attempts == 1);
	}
	frontiers_received = true;
}

void oslo::bootstrap_attempt_legacy::run ()
{
	debug_assert (started);
	debug_assert (!node->flags.disable_legacy_bootstrap);
	node->bootstrap_initiator.connections->populate_connections (false);
	oslo::unique_lock<std::mutex> lock (mutex);
	run_start (lock);
	while (still_pulling ())
	{
		while (still_pulling ())
		{
			// clang-format off
			condition.wait (lock, [&stopped = stopped, &pulling = pulling, &frontiers_confirmation_pending = frontiers_confirmation_pending] { return stopped || pulling == 0 || frontiers_confirmation_pending; });
			// clang-format on
			attempt_restart_check (lock);
		}
		// Flushing may resolve forks which can add more pulls
		node->logger.try_log ("Flushing unchecked blocks");
		lock.unlock ();
		node->block_processor.flush ();
		lock.lock ();
		node->logger.try_log ("Finished flushing unchecked blocks");
	}
	if (!stopped)
	{
		node->logger.try_log ("Completed legacy pulls");
		if (!node->flags.disable_bootstrap_bulk_push_client)
		{
			request_push (lock);
		}
		if (!stopped)
		{
			node->unchecked_cleanup ();
		}
	}
	lock.unlock ();
	stop ();
	condition.notify_all ();
}

void oslo::bootstrap_attempt_legacy::get_information (boost::property_tree::ptree & tree_a)
{
	oslo::lock_guard<std::mutex> lock (mutex);
	tree_a.put ("frontier_pulls", std::to_string (frontier_pulls.size ()));
	tree_a.put ("frontiers_received", static_cast<bool> (frontiers_received));
	tree_a.put ("frontiers_confirmed", static_cast<bool> (frontiers_confirmed));
	tree_a.put ("frontiers_confirmation_pending", static_cast<bool> (frontiers_confirmation_pending));
}
