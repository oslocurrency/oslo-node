#pragma once

#include <oslo/node/network.hpp>
#include <oslo/node/repcrawler.hpp>

#include <unordered_map>

namespace oslo
{
class election;
class node;
/** This class accepts elections that need further votes before they can be confirmed and bundles them in to single confirm_req packets */
class confirmation_solicitor final
{
public:
	confirmation_solicitor (oslo::network &, oslo::network_constants const &);
	/** Prepare object for batching election confirmation requests*/
	void prepare (std::vector<oslo::representative> const &);
	/** Broadcast the winner of an election if the broadcast limit has not been reached. Returns false if the broadcast was performed */
	bool broadcast (oslo::election const &);
	/** Add an election that needs to be confirmed. Returns false if successfully added */
	bool add (oslo::election const &);
	/** Dispatch bundled requests to each channel*/
	void flush ();
	/** Maximum amount of confirmation requests (batches) to be sent to each channel */
	size_t const max_confirm_req_batches;
	/** Global maximum amount of block broadcasts */
	size_t const max_block_broadcasts;
	/** Maximum amount of requests to be sent per election */
	size_t const max_election_requests;
	/** Maximum amount of directed broadcasts to be sent per election */
	size_t const max_election_broadcasts;

private:
	oslo::network & network;

	unsigned rebroadcasted{ 0 };
	std::vector<oslo::representative> representatives_requests;
	std::vector<oslo::representative> representatives_broadcasts;
	using vector_root_hashes = std::vector<std::pair<oslo::block_hash, oslo::root>>;
	std::unordered_map<std::shared_ptr<oslo::transport::channel>, vector_root_hashes> requests;
	bool prepared{ false };
};
}
