#pragma once

#include <charconv>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <string_view>

namespace mount_options {

namespace {

template <typename U>
struct extract_base {
	using type = U;
};

template <typename U>
struct extract_base<std::optional<U>> {
	using type = U;
};

template <typename U>
using extract_base_t = typename extract_base<U>::type;

} // namespace

template <std::integral T, int Base = 10>
std::expected<T, std::string> parse_numeric(std::string_view val) {
	T parsed_val;
	auto [p, ec] = std::from_chars(val.data(), val.data() + val.size(), parsed_val, Base);

	if (ec == std::errc() && p == val.data() + val.size()) {
		return parsed_val;
	}
	return std::unexpected(std::format("invalid format: '{}'", val));
}

std::expected<std::string, std::string> parse_string(std::string_view view) {
	return std::string{view};
}

template <typename F, typename Target>
    requires(std::is_invocable_r_v<std::expected<extract_base_t<Target>, std::string>, F, std::string_view>)
struct MountOption {
	std::string_view name;
	F parser;
	std::reference_wrapper<Target> target;
};

template <typename F, typename Target>
MountOption(std::string_view, F, std::reference_wrapper<Target>) -> MountOption<F, Target>;

template <typename... Opts>
std::expected<void, std::string>
parse(std::string_view opts_str, const std::tuple<Opts...> &options) {
	if (opts_str.empty())
		return {};

	for (const auto token_range : opts_str | std::views::split(',')) {
		std::string_view token(token_range);
		if (token.empty())
			continue;

		auto kv_view = token | std::views::split('=');
		auto kv_it = kv_view.cbegin();
		if (kv_it == kv_view.cend())
			continue;

		std::string_view key(*kv_it);
		std::string_view value;
		if (++kv_it != kv_view.cend())
			value = std::string_view(*kv_it);

		bool found = false;
		std::expected<void, std::string> parse_err;

		std::apply(
		    [&](const auto &...tuple_opts) {
			    auto check_opt = [&](const auto &opt) {
				    if (found || opt.name != key)
					    return;

					found = true;
					if (auto parse_result = opt.parser(value); parse_result)
						opt.target.get() = parse_result.value();
					else
						parse_err = std::unexpected(parse_result.error());
			    };

			    (check_opt(tuple_opts), ...);
		    },
		    options
		);

		if (!found)
			return std::unexpected(std::format("unknown mount option: '{}'", key));
		if (!parse_err)
			return parse_err;
	}

	return {};
}

} // namespace mount_options
