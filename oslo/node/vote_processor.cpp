#include <oslo/lib/logger_mt.hpp>
#include <oslo/lib/stats.hpp>
#include <oslo/lib/threading.hpp>
#include <oslo/lib/timer.hpp>
#include <oslo/node/active_transactions.hpp>
#include <oslo/node/node.hpp>
#include <oslo/node/node_observers.hpp>
#include <oslo/node/nodeconfig.hpp>
#include <oslo/node/online_reps.hpp>
#include <oslo/node/signatures.hpp>
#include <oslo/node/vote_processor.hpp>
#include <oslo/secure/blockstore.hpp>
#include <oslo/secure/common.hpp>
#include <oslo/secure/ledger.hpp>

#include <boost/format.hpp>

oslo::vote_processor::vote_processor (oslo::signature_checker & checker_a, oslo::active_transactions & active_a, oslo::node_observers & observers_a, oslo::stat & stats_a, oslo::node_config & config_a, oslo::node_flags & flags_a, oslo::logger_mt & logger_a, oslo::online_reps & online_reps_a, oslo::ledger & ledger_a, oslo::network_params & network_params_a) :
checker (checker_a),
active (active_a),
observers (observers_a),
stats (stats_a),
config (config_a),
logger (logger_a),
online_reps (online_reps_a),
ledger (ledger_a),
network_params (network_params_a),
max_votes (flags_a.vote_processor_capacity),
started (false),
stopped (false),
is_active (false),
thread ([this]() {
	oslo::thread_role::set (oslo::thread_role::name::vote_processing);
	process_loop ();
})
{
	oslo::unique_lock<std::mutex> lock (mutex);
	condition.wait (lock, [& started = started] { return started; });
}

void oslo::vote_processor::process_loop ()
{
	oslo::timer<std::chrono::milliseconds> elapsed;
	bool log_this_iteration;

	oslo::unique_lock<std::mutex> lock (mutex);
	started = true;

	lock.unlock ();
	condition.notify_all ();
	lock.lock ();

	while (!stopped)
	{
		if (!votes.empty ())
		{
			decltype (votes) votes_l;
			votes_l.swap (votes);

			log_this_iteration = false;
			if (config.logging.network_logging () && votes_l.size () > 50)
			{
				/*
				 * Only log the timing information for this iteration if
				 * there are a sufficient number of items for it to be relevant
				 */
				log_this_iteration = true;
				elapsed.restart ();
			}
			is_active = true;
			lock.unlock ();
			verify_votes (votes_l);
			lock.lock ();
			is_active = false;

			lock.unlock ();
			condition.notify_all ();
			lock.lock ();

			if (log_this_iteration && elapsed.stop () > std::chrono::milliseconds (100))
			{
				logger.try_log (boost::str (boost::format ("Processed %1% votes in %2% milliseconds (rate of %3% votes per second)") % votes_l.size () % elapsed.value ().count () % ((votes_l.size () * 1000ULL) / elapsed.value ().count ())));
			}
		}
		else
		{
			condition.wait (lock);
		}
	}
}

bool oslo::vote_processor::vote (std::shared_ptr<oslo::vote> vote_a, std::shared_ptr<oslo::transport::channel> channel_a)
{
	bool process (false);
	oslo::unique_lock<std::mutex> lock (mutex);
	if (!stopped)
	{
		// Level 0 (< 0.1%)
		if (votes.size () < 6.0 / 9.0 * max_votes)
		{
			process = true;
		}
		// Level 1 (0.1-1%)
		else if (votes.size () < 7.0 / 9.0 * max_votes)
		{
			process = (representatives_1.find (vote_a->account) != representatives_1.end ());
		}
		// Level 2 (1-5%)
		else if (votes.size () < 8.0 / 9.0 * max_votes)
		{
			process = (representatives_2.find (vote_a->account) != representatives_2.end ());
		}
		// Level 3 (> 5%)
		else if (votes.size () < max_votes)
		{
			process = (representatives_3.find (vote_a->account) != representatives_3.end ());
		}
		if (process)
		{
			votes.emplace_back (vote_a, channel_a);
			lock.unlock ();
			condition.notify_all ();
			// Lock no longer required
		}
		else
		{
			stats.inc (oslo::stat::type::vote, oslo::stat::detail::vote_overflow);
		}
	}
	return !process;
}

void oslo::vote_processor::verify_votes (decltype (votes) const & votes_a)
{
	auto size (votes_a.size ());
	std::vector<unsigned char const *> messages;
	messages.reserve (size);
	std::vector<oslo::block_hash> hashes;
	hashes.reserve (size);
	std::vector<size_t> lengths (size, sizeof (oslo::block_hash));
	std::vector<unsigned char const *> pub_keys;
	pub_keys.reserve (size);
	std::vector<unsigned char const *> signatures;
	signatures.reserve (size);
	std::vector<int> verifications;
	verifications.resize (size);
	for (auto const & vote : votes_a)
	{
		hashes.push_back (vote.first->hash ());
		messages.push_back (hashes.back ().bytes.data ());
		pub_keys.push_back (vote.first->account.bytes.data ());
		signatures.push_back (vote.first->signature.bytes.data ());
	}
	oslo::signature_check_set check = { size, messages.data (), lengths.data (), pub_keys.data (), signatures.data (), verifications.data () };
	checker.verify (check);
	auto i (0);
	for (auto const & vote : votes_a)
	{
		debug_assert (verifications[i] == 1 || verifications[i] == 0);
		if (verifications[i] == 1)
		{
			vote_blocking (vote.first, vote.second, true);
		}
		++i;
	}
}

oslo::vote_code oslo::vote_processor::vote_blocking (std::shared_ptr<oslo::vote> vote_a, std::shared_ptr<oslo::transport::channel> channel_a, bool validated)
{
	auto result (oslo::vote_code::invalid);
	if (validated || !vote_a->validate ())
	{
		result = active.vote (vote_a);
		observers.vote.notify (vote_a, channel_a, result);
	}
	std::string status;
	switch (result)
	{
		case oslo::vote_code::invalid:
			status = "Invalid";
			stats.inc (oslo::stat::type::vote, oslo::stat::detail::vote_invalid);
			break;
		case oslo::vote_code::replay:
			status = "Replay";
			stats.inc (oslo::stat::type::vote, oslo::stat::detail::vote_replay);
			break;
		case oslo::vote_code::vote:
			status = "Vote";
			stats.inc (oslo::stat::type::vote, oslo::stat::detail::vote_valid);
			break;
		case oslo::vote_code::indeterminate:
			status = "Indeterminate";
			stats.inc (oslo::stat::type::vote, oslo::stat::detail::vote_indeterminate);
			break;
	}
	if (config.logging.vote_logging ())
	{
		logger.try_log (boost::str (boost::format ("Vote from: %1% sequence: %2% block(s): %3%status: %4%") % vote_a->account.to_account () % std::to_string (vote_a->sequence) % vote_a->hashes_string () % status));
	}
	return result;
}

void oslo::vote_processor::stop ()
{
	{
		oslo::lock_guard<std::mutex> lock (mutex);
		stopped = true;
	}
	condition.notify_all ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

void oslo::vote_processor::flush ()
{
	oslo::unique_lock<std::mutex> lock (mutex);
	while (is_active || !votes.empty ())
	{
		condition.wait (lock);
	}
}

size_t oslo::vote_processor::size ()
{
	oslo::lock_guard<std::mutex> guard (mutex);
	return votes.size ();
}

bool oslo::vote_processor::empty ()
{
	oslo::lock_guard<std::mutex> guard (mutex);
	return votes.empty ();
}

void oslo::vote_processor::calculate_weights ()
{
	oslo::unique_lock<std::mutex> lock (mutex);
	if (!stopped)
	{
		representatives_1.clear ();
		representatives_2.clear ();
		representatives_3.clear ();
		auto supply (online_reps.online_stake ());
		auto rep_amounts = ledger.cache.rep_weights.get_rep_amounts ();
		for (auto const & rep_amount : rep_amounts)
		{
			oslo::account const & representative (rep_amount.first);
			auto weight (ledger.weight (representative));
			if (weight > supply / 1000) // 0.1% or above (level 1)
			{
				representatives_1.insert (representative);
				if (weight > supply / 100) // 1% or above (level 2)
				{
					representatives_2.insert (representative);
					if (weight > supply / 20) // 5% or above (level 3)
					{
						representatives_3.insert (representative);
					}
				}
			}
		}
	}
}

std::unique_ptr<oslo::container_info_component> oslo::collect_container_info (vote_processor & vote_processor, const std::string & name)
{
	size_t votes_count;
	size_t representatives_1_count;
	size_t representatives_2_count;
	size_t representatives_3_count;

	{
		oslo::lock_guard<std::mutex> guard (vote_processor.mutex);
		votes_count = vote_processor.votes.size ();
		representatives_1_count = vote_processor.representatives_1.size ();
		representatives_2_count = vote_processor.representatives_2.size ();
		representatives_3_count = vote_processor.representatives_3.size ();
	}

	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "votes", votes_count, sizeof (decltype (vote_processor.votes)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "representatives_1", representatives_1_count, sizeof (decltype (vote_processor.representatives_1)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "representatives_2", representatives_2_count, sizeof (decltype (vote_processor.representatives_2)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "representatives_3", representatives_3_count, sizeof (decltype (vote_processor.representatives_3)::value_type) }));
	return composite;
}
