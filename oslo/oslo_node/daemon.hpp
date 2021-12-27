namespace boost
{
namespace filesystem
{
	class path;
}
}

namespace oslo
{
class node_flags;
}
namespace oslo_daemon
{
class daemon
{
public:
	void run (boost::filesystem::path const &, oslo::node_flags const & flags);
};
}
