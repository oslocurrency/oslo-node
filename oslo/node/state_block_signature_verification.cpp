#include <oslo/lib/logger_mt.hpp>
#include <oslo/lib/numbers.hpp>
#include <oslo/lib/threading.hpp>
#include <oslo/node/nodeconfig.hpp>
#include <oslo/node/signatures.hpp>
#include <oslo/node/state_block_signature_verification.hpp>
#include <oslo/secure/common.hpp>

#include <boost/format.hpp>

oslo::state_block_signature_verification::state_block_signature_verification (oslo::signature_checker & signature_checker, oslo::epochs & epochs, oslo::node_config & node_config, oslo::logger_mt & logger, uint64_t state_block_signature_verification_size) :
signature_checker (signature_checker),
epochs (epochs),
node_config (node_config),
logger (logger),
thread ([this, state_block_signature_verification_size]() {
	oslo::thread_role::set (oslo::thread_role::name::state_block_signature_verification);
	this->run (state_block_signature_verification_size);
})
{
}

oslo::state_block_signature_verification::~state_block_signature_verification ()
{
	stop ();
}

void oslo::state_block_signature_verification::stop ()
{
	{
		oslo::lock_guard<std::mutex> guard (mutex);
		stopped = true;
	}

	if (thread.joinable ())
	{
		condition.notify_one ();
		thread.join ();
	}
}

void oslo::state_block_signature_verification::run (uint64_t state_block_signature_verification_size)
{
	oslo::unique_lock<std::mutex> lk (mutex);
	while (!stopped)
	{
		if (!state_blocks.empty ())
		{
			size_t const max_verification_batch (state_block_signature_verification_size != 0 ? state_block_signature_verification_size : oslo::signature_checker::batch_size * (node_config.signature_checker_threads + 1));
			active = true;
			while (!state_blocks.empty () && !stopped)
			{
				auto items = setup_items (max_verification_batch);
				lk.unlock ();
				verify_state_blocks (items);
				lk.lock ();
			}
			active = false;
			lk.unlock ();
			transition_inactive_callback ();
			lk.lock ();
		}
		else
		{
			condition.wait (lk);
		}
	}
}

bool oslo::state_block_signature_verification::is_active ()
{
	oslo::lock_guard<std::mutex> guard (mutex);
	return active;
}

void oslo::state_block_signature_verification::add (oslo::unchecked_info const & info_a)
{
	{
		oslo::lock_guard<std::mutex> guard (mutex);
		state_blocks.push_back (info_a);
	}
	condition.notify_one ();
}

size_t oslo::state_block_signature_verification::size ()
{
	oslo::lock_guard<std::mutex> guard (mutex);
	return state_blocks.size ();
}

std::deque<oslo::unchecked_info> oslo::state_block_signature_verification::setup_items (size_t max_count)
{
	std::deque<oslo::unchecked_info> items;
	if (state_blocks.size () <= max_count)
	{
		items.swap (state_blocks);
	}
	else
	{
		for (auto i (0); i < max_count; ++i)
		{
			items.push_back (state_blocks.front ());
			state_blocks.pop_front ();
		}
		debug_assert (!state_blocks.empty ());
	}
	return items;
}

void oslo::state_block_signature_verification::verify_state_blocks (std::deque<oslo::unchecked_info> & items)
{
	if (!items.empty ())
	{
		oslo::timer<> timer_l;
		timer_l.start ();
		auto size (items.size ());
		std::vector<oslo::block_hash> hashes;
		hashes.reserve (size);
		std::vector<unsigned char const *> messages;
		messages.reserve (size);
		std::vector<size_t> lengths;
		lengths.reserve (size);
		std::vector<oslo::account> accounts;
		accounts.reserve (size);
		std::vector<unsigned char const *> pub_keys;
		pub_keys.reserve (size);
		std::vector<oslo::signature> blocks_signatures;
		blocks_signatures.reserve (size);
		std::vector<unsigned char const *> signatures;
		signatures.reserve (size);
		std::vector<int> verifications;
		verifications.resize (size, 0);
		for (auto & item : items)
		{
			hashes.push_back (item.block->hash ());
			messages.push_back (hashes.back ().bytes.data ());
			lengths.push_back (sizeof (decltype (hashes)::value_type));
			oslo::account account (item.block->account ());
			if (!item.block->link ().is_zero () && epochs.is_epoch_link (item.block->link ()))
			{
				account = epochs.signer (epochs.epoch (item.block->link ()));
			}
			else if (!item.account.is_zero ())
			{
				account = item.account;
			}
			accounts.push_back (account);
			pub_keys.push_back (accounts.back ().bytes.data ());
			blocks_signatures.push_back (item.block->block_signature ());
			signatures.push_back (blocks_signatures.back ().bytes.data ());
		}
		oslo::signature_check_set check = { size, messages.data (), lengths.data (), pub_keys.data (), signatures.data (), verifications.data () };
		signature_checker.verify (check);
		if (node_config.logging.timing_logging () && timer_l.stop () > std::chrono::milliseconds (10))
		{
			logger.try_log (boost::str (boost::format ("Batch verified %1% state blocks in %2% %3%") % size % timer_l.value ().count () % timer_l.unit ()));
		}
		blocks_verified_callback (items, verifications, hashes, blocks_signatures);
	}
}

std::unique_ptr<oslo::container_info_component> oslo::collect_container_info (state_block_signature_verification & state_block_signature_verification, const std::string & name)
{
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "state_blocks", state_block_signature_verification.size (), sizeof (oslo::unchecked_info) }));
	return composite;
}
