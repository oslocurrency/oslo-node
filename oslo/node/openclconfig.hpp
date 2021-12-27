#pragma once

#include <oslo/lib/errors.hpp>

namespace oslo
{
class jsonconfig;
class tomlconfig;
class opencl_config
{
public:
	opencl_config () = default;
	opencl_config (unsigned, unsigned, unsigned);
	oslo::error serialize_json (oslo::jsonconfig &) const;
	oslo::error deserialize_json (oslo::jsonconfig &);
	oslo::error serialize_toml (oslo::tomlconfig &) const;
	oslo::error deserialize_toml (oslo::tomlconfig &);
	unsigned platform{ 0 };
	unsigned device{ 0 };
	unsigned threads{ 1024 * 1024 };
};
}
