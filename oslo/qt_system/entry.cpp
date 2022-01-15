#include <oslo/crypto_lib/random_pool.hpp>
#include <oslo/lib/config.hpp>
#include <oslo/lib/threading.hpp>
#include <oslo/node/common.hpp>
#include <oslo/node/testing.hpp>
#include <oslo/qt/qt.hpp>

#include <boost/format.hpp>

#include <thread>

int main (int argc, char ** argv)
{
	oslo::network_constants::set_active_network (oslo::oslo_networks::oslo_test_network);
	oslo::node_singleton_memory_pool_purge_guard memory_pool_cleanup_guard;
	QApplication application (argc, argv);
	QCoreApplication::setOrganizationName ("Oslo");
	QCoreApplication::setOrganizationDomain ("oslo.org");
	QCoreApplication::setApplicationName ("Oslo Wallet");
	oslo_qt::eventloop_processor processor;
	const uint16_t count (16);
	oslo::system system (count);
	oslo::thread_runner runner (system.io_ctx, system.nodes[0]->config.io_threads);
	std::unique_ptr<QTabWidget> client_tabs (new QTabWidget);
	std::vector<std::unique_ptr<oslo_qt::wallet>> guis;
	for (auto i (0); i < count; ++i)
	{
		auto wallet (system.nodes[i]->wallets.create (oslo::random_wallet_id ()));
		oslo::keypair key;
		wallet->insert_adhoc (key.prv);
		guis.push_back (std::make_unique<oslo_qt::wallet> (application, processor, *system.nodes[i], wallet, key.pub));
		client_tabs->addTab (guis.back ()->client_window, boost::str (boost::format ("Wallet %1%") % i).c_str ());
	}
	client_tabs->show ();
	QObject::connect (&application, &QApplication::aboutToQuit, [&]() {
		system.stop ();
	});
	int result;
	try
	{
		result = application.exec ();
	}
	catch (...)
	{
		result = -1;
		debug_assert (false);
	}
	runner.join ();
	return result;
}
