// Copyright (c) 2023, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include <algorithm>
#include <cassert>
#include <charconv>
#include <iterator>
#include <optional>
#include <tuple>

#include "mamba/core/error_handling.hpp"
#include "mamba/core/util_string.hpp"
#include "mamba/specs/version.hpp"
#include "mamba/util/cast.hpp"

namespace mamba::specs
{
    namespace
    {
        // TODO(C++20) use operator<=>
        enum strong_ordering
        {
            less,
            equal,
            greater
        };

        template <typename T>
        auto compare_three_way(const T& a, const T& b) -> strong_ordering
        {
            if (a < b)
            {
                return strong_ordering::less;
            }
            if (a == b)
            {
                return strong_ordering::equal;
            }
            return strong_ordering::greater;
        }

        template <>
        auto compare_three_way(const std::string& a, const std::string& b) -> strong_ordering
        {
            return compare_three_way(std::strcmp(a.c_str(), b.c_str()), 0);
        }
    }

    /***************************************
     *  Implementation of VersionPartAtom  *
     ***************************************/

    VersionPartAtom::VersionPartAtom(std::size_t numeral) noexcept
        : m_numeral{ numeral }
    {
    }

    VersionPartAtom::VersionPartAtom(std::size_t numeral, std::string_view literal)
        : m_literal{ to_lower(literal) }
        , m_numeral{ numeral }
    {
    }

    template <typename Char>
    VersionPartAtom::VersionPartAtom(std::size_t numeral, std::basic_string<Char>&& literal)
        : m_literal{ to_lower(std::move(literal)) }
        , m_numeral{ numeral }
    {
    }

    template VersionPartAtom::VersionPartAtom(std::size_t, std::string&&);

    auto VersionPartAtom::numeral() const noexcept -> std::size_t
    {
        return m_numeral;
    }

    auto VersionPartAtom::literal() const& noexcept -> const std::string&
    {
        return m_literal;
    }

    auto VersionPartAtom::literal() && noexcept -> std::string
    {
        return std::move(m_literal);
    }

    auto VersionPartAtom::str() const -> std::string
    {
        return fmt::format("{}", *this);
    }

    namespace
    {
        template <>
        auto compare_three_way(const VersionPartAtom& a, const VersionPartAtom& b) -> strong_ordering
        {
            const auto num_ord = compare_three_way(a.numeral(), b.numeral());
            if (num_ord != strong_ordering::equal)
            {
                return num_ord;
            }

            // Certain literals have sepcial meaning we map then to a priority
            // 0 meaning regular string
            auto lit_priority = [](const auto& l) -> int
            {
                if (l == "*")
                {
                    return -3;
                }
                if (l == "dev")
                {
                    return -2;
                }
                if (l == "_")
                {
                    return -1;
                }
                if (l == "")
                {
                    return 1;
                }
                if (l == "post")
                {
                    return 2;
                }
                return 0;
            };
            const auto a_lit_val = lit_priority(a.literal());
            const auto b_lit_val = lit_priority(b.literal());
            // If two regular string, we need to use string comparison
            if ((a_lit_val == 0) && (b_lit_val == 0))
            {
                return compare_three_way<std::string>(a.literal(), b.literal());
            }
            return compare_three_way(a_lit_val, b_lit_val);
        }
    }

    auto VersionPartAtom::operator==(const VersionPartAtom& other) const -> bool
    {
        // More efficient thatn three way comparison because of edge cases
        auto attrs = [](const VersionPartAtom& a) -> std::tuple<std::size_t, const std::string&> {
            return { a.numeral(), a.literal() };
        };
        return attrs(*this) == attrs(other);
    }

    auto VersionPartAtom::operator!=(const VersionPartAtom& other) const -> bool
    {
        // More efficient thatn three way comparison
        return !(*this == other);
    }

    auto VersionPartAtom::operator<(const VersionPartAtom& other) const -> bool
    {
        return compare_three_way(*this, other) == strong_ordering::less;
    }

    auto VersionPartAtom::operator<=(const VersionPartAtom& other) const -> bool
    {
        return compare_three_way(*this, other) != strong_ordering::greater;
    }

    auto VersionPartAtom::operator>(const VersionPartAtom& other) const -> bool
    {
        return compare_three_way(*this, other) == strong_ordering::greater;
    }

    auto VersionPartAtom::operator>=(const VersionPartAtom& other) const -> bool
    {
        return compare_three_way(*this, other) != strong_ordering::less;
    }

    /*******************************
     *  Implementation of Version  *
     *******************************/

    Version::Version(std::size_t epoch, CommonVersion&& version, CommonVersion&& local) noexcept
        : m_version{ std::move(version) }
        , m_local{ std::move(local) }
        , m_epoch{ epoch }
    {
    }

    auto Version::epoch() const noexcept -> std::size_t
    {
        return m_epoch;
    }

    auto Version::version() const noexcept -> const CommonVersion&
    {
        return m_version;
    }

    auto Version::local() const noexcept -> const CommonVersion&
    {
        return m_local;
    }

    auto Version::str() const -> std::string
    {
        return fmt::format("{}", *this);
    }

    namespace
    {
        /**
         * Compare two range where some trailing elements can be considered as empty.
         *
         * If ``0`` is considered "empty" then all the ranges ``[1, 2] and ``[1, 2, 0]``,
         * ``[1, 2, 0, 0]`` are considered equal, however ``[1, 2]`` and ``[1, 0, 2]`` are not.
         * Similarily ``[1, 1] is less than ``[1, 2, 0]`` but more than ``[1, 1, -1]``
         * because ``-1 < 0``.
         *
         * @return The comparison between the two sequence
         * @return The first index where the two sequence diverge.
         */
        template <typename Iter1, typename Iter2, typename Empty1, typename Empty2, typename Cmp>
        constexpr auto lexicographical_compare_three_way_trailing(
            Iter1 first1,
            Iter1 last1,
            Iter2 first2,
            Iter2 last2,
            const Empty1& empty1,
            const Empty2& empty2,
            Cmp comp
        ) -> std::pair<strong_ordering, std::size_t>
        {
            assert(std::distance(first1, last1) >= 0);
            assert(std::distance(first2, last2) >= 0);

            auto iter1 = first1;
            auto iter2 = first2;
            for (; (iter1 != last1) && (iter2 != last2); ++iter1, ++iter2)
            {
                if (auto c = comp(*iter1, *iter2); c != strong_ordering::equal)
                {
                    return { c, static_cast<std::size_t>(std::distance(first1, iter1)) };
                }
            }

            // They have the same leading elements but 1 has more elements
            // We do a lexicographic compare with an infite sequence of empties
            if ((iter1 != last1))
            {
                for (; iter1 != last1; ++iter1)
                {
                    if (auto c = comp(*iter1, empty2); c != strong_ordering::equal)
                    {
                        return { c, static_cast<std::size_t>(std::distance(first1, iter1)) };
                    }
                }
            }
            // first2 != last2
            // They have the same leading elements but 2 has more elements
            // We do a lexicographic compare with an infite sequence of empties
            if ((iter2 != last2))
            {
                for (; iter2 != last2; ++iter2)
                {
                    if (auto c = comp(empty1, *iter2); c != strong_ordering::equal)
                    {
                        return { c, static_cast<std::size_t>(std::distance(first2, iter2)) };
                    }
                }
            }
            // They have the same elements
            return { strong_ordering::equal, static_cast<std::size_t>(std::distance(first1, iter1)) };
        }

        template <typename Iter1, typename Iter2, typename Empty, typename Cmp>
        constexpr auto lexicographical_compare_three_way_trailing(
            Iter1 first1,
            Iter1 last1,
            Iter2 first2,
            Iter2 last2,
            const Empty& empty,
            Cmp comp
        ) -> std::pair<strong_ordering, std::size_t>
        {
            return lexicographical_compare_three_way_trailing(
                first1,
                last1,
                first2,
                last2,
                empty,
                empty,
                comp
            );
        }

        template <>
        auto compare_three_way(const VersionPart& a, const VersionPart& b) -> strong_ordering
        {
            return lexicographical_compare_three_way_trailing(
                       a.cbegin(),
                       a.cend(),
                       b.cbegin(),
                       b.cend(),
                       VersionPartAtom{},
                       [](const auto& x, const auto& y) { return compare_three_way(x, y); }
            ).first;
        }

        template <>
        auto compare_three_way(const CommonVersion& a, const CommonVersion& b) -> strong_ordering
        {
            return lexicographical_compare_three_way_trailing(
                       a.cbegin(),
                       a.cend(),
                       b.cbegin(),
                       b.cend(),
                       VersionPart{},
                       [](const auto& x, const auto& y) { return compare_three_way(x, y); }
            ).first;
        }

        template <>
        auto compare_three_way(const Version& a, const Version& b) -> strong_ordering
        {
            if (auto c = compare_three_way(a.epoch(), b.epoch()); c != strong_ordering::equal)
            {
                return c;
            }
            if (auto c = compare_three_way(a.version(), b.version()); c != strong_ordering::equal)
            {
                return c;
            }
            return compare_three_way(a.local(), b.local());
        }
    }

    // TODO(C++20) use operator<=> to simplify code and improve operator<=
    auto Version::operator==(const Version& other) const -> bool
    {
        return compare_three_way(*this, other) == strong_ordering::equal;
    }

    auto Version::operator!=(const Version& other) const -> bool
    {
        return !(*this == other);
    }

    auto Version::operator<(const Version& other) const -> bool
    {
        return compare_three_way(*this, other) == strong_ordering::less;
    }

    auto Version::operator<=(const Version& other) const -> bool
    {
        return compare_three_way(*this, other) != strong_ordering::greater;
    }

    auto Version::operator>(const Version& other) const -> bool
    {
        return compare_three_way(*this, other) == strong_ordering::greater;
    }

    auto Version::operator>=(const Version& other) const -> bool
    {
        return compare_three_way(*this, other) != strong_ordering::less;
    }

    namespace
    {
        struct AlwaysEqual
        {
        };

        [[maybe_unused]] auto starts_with_three_way(const AlwaysEqual&, const AlwaysEqual&)
            -> strong_ordering
        {
            // This comparison should not happen with the current usage.
            assert(false);
            return strong_ordering::equal;
        }

        template <typename T>
        auto starts_with_three_way(const AlwaysEqual&, const T&) -> strong_ordering
        {
            return strong_ordering::equal;
        }

        template <typename T>
        auto starts_with_three_way(const T&, const AlwaysEqual&) -> strong_ordering
        {
            return strong_ordering::equal;
        }

        auto starts_with_three_way(const VersionPartAtom& a, const VersionPartAtom& b)
            -> strong_ordering
        {
            if ((a.numeral() == b.numeral()) && b.literal().empty())
            {
                return strong_ordering::equal;
            }
            return compare_three_way(a, b);
        }

        auto starts_with_three_way(const VersionPart& a, const VersionPart& b) -> strong_ordering
        {
            return lexicographical_compare_three_way_trailing(
                       a.cbegin(),
                       a.cend(),
                       b.cbegin(),
                       b.cend(),
                       VersionPartAtom{},
                       AlwaysEqual{},
                       [](const auto& x, const auto& y) { return starts_with_three_way(x, y); }
            ).first;
        }

        auto starts_with_three_way(const CommonVersion& a, const CommonVersion& b) -> strong_ordering
        {
            return lexicographical_compare_three_way_trailing(
                       a.cbegin(),
                       a.cend(),
                       b.cbegin(),
                       b.cend(),
                       VersionPart{},
                       AlwaysEqual{},
                       [](const auto& x, const auto& y) { return starts_with_three_way(x, y); }
            ).first;
        }

        auto starts_with_three_way(const Version& a, const Version& b) -> strong_ordering
        {
            if (auto c = compare_three_way(a.epoch(), b.epoch()); c != strong_ordering::equal)
            {
                return c;
            }
            if (auto c = starts_with_three_way(a.version(), b.version()); c != strong_ordering::equal)
            {
                return c;
            }
            return compare_three_way(a.local(), b.local());
        }
    }

    auto Version::starts_with(const Version& prefix) const -> bool
    {
        return starts_with_three_way(*this, prefix) == strong_ordering::equal;
    }

    namespace
    {
        auto
        compatible_with_impl(const CommonVersion& newer, const CommonVersion& older, std::size_t level)
            -> bool
        {
            auto [cmp, idx] = lexicographical_compare_three_way_trailing(
                newer.cbegin(),
                newer.cend(),
                older.cbegin(),
                older.cend(),
                VersionPart{},
                [](const auto& x, const auto& y) { return compare_three_way(x, y); }
            );

            return (cmp == strong_ordering::equal)
                   || ((cmp == strong_ordering::greater) && (idx >= level));
        }
    }

    auto Version::compatible_with(const Version& older, std::size_t level) const -> bool
    {
        return (epoch() == older.epoch()) && compatible_with_impl(version(), older.version(), level)
               && compatible_with_impl(local(), older.local(), level);
    }

    namespace
    {
        // TODO(C++20) This is a std::string_view constructor
        template <typename Iter>
        auto make_string_view(Iter first, Iter last) -> std::string_view
        {
            const auto size = util::safe_num_cast<std::size_t>(last - first);
            return { first, size };
        }

        template <typename Int>
        auto to_int(std::string_view str) -> std::optional<Int>
        {
            Int val = 0;
            const auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), val);
            if ((ec != std::errc()) || (ptr != (str.data() + str.size())))  // TODO(C++20)
                                                                            // [[unlikely]]
            {
                return {};
            }
            return val;
        }

        template <typename Int>
        auto parse_leading_epoch(std::string_view str) -> std::pair<Int, std::string_view>
        {
            const auto delim_pos = str.find(Version::epoch_delim);
            // No epoch is specified
            if (delim_pos == std::string_view::npos)  // TODO(C++20) [[likely]]
            {
                return { Int(0), str };
            }
            if (delim_pos == 0)
            {
                throw std::invalid_argument(
                    fmt::format("Empty epoch delimited by '{}'.", Version::epoch_delim)
                );
            }

            const auto epoch_str = str.substr(0, delim_pos);
            const auto maybe_int = to_int<Int>(epoch_str);
            // Epoch is not a number (or empty)
            if (!maybe_int.has_value())
            {
                throw std::invalid_argument(
                    fmt::format("Epoch should be a number, got '{}'.", epoch_str)
                );
            }
            // Found an epoch
            return { maybe_int.value(), str.substr(delim_pos + 1) };
        }

        template <typename Int>
        auto parse_leading_integer(std::string_view str) -> std::pair<Int, std::string_view>
        {
            const auto [integer_str, rest] = lstrip_if_parts(str, [](char c) { return is_digit(c); });
            auto maybe_integer = to_int<Int>(integer_str);
            assert(maybe_integer.has_value());
            return { maybe_integer.value(), rest };
        }

        auto parse_leading_literal(std::string_view str)
            -> std::pair<std::string_view, std::string_view>
        {
            const auto [literal, rest] = lstrip_if_parts(str, [](char c) { return !is_digit(c); });
            return { literal, rest };
        }

        auto parse_leading_part_atom(std::string_view str)
            -> std::pair<VersionPartAtom, std::string_view>
        {
            assert(!str.empty());

            std::size_t numeral = 0;
            std::string_view literal = {};
            auto tail = std::string_view{};
            if (is_digit(str.front()))
            {
                std::tie(numeral, tail) = parse_leading_integer<std::size_t>(str);
            }
            else
            {
                tail = str;
            }
            std::tie(literal, tail) = parse_leading_literal(tail);
            return { { numeral, literal }, tail };
        }

        auto parse_part(std::string_view str) -> VersionPart
        {
            assert(!str.empty());

            VersionPart atoms = {};
            while (!str.empty())
            {
                atoms.emplace_back();
                std::tie(atoms.back(), str) = parse_leading_part_atom(str);
            }
            return atoms;
        }

        void check_common_version(std::string_view str)
        {
            // `_` and `-` delimiter cannot be used together.
            // Special meaning for `_` at the end of the string.
            if ((str.find(Version::part_delim_alt) < str.size())
                && (str.find(Version::part_delim_special) < str.size() - 1))  // TODO(C++20)
                                                                              // [[unlikely]]
            {
                throw std::invalid_argument(fmt::format(
                    "Cannot use both '{}' and '{}' delimiters in {}'.",
                    Version::part_delim_alt,
                    Version::part_delim_special,
                    str
                ));
            }

            auto allowed_char = [](char c) -> bool
            {
                return is_alphanum(c)                         //
                       || (c == Version::part_delim)          //
                       || (c == Version::part_delim_alt)      //
                       || (c == Version::part_delim_special)  //
                       || (c == '*');                         // Weirdly but it is in conda tests
            };
            if (std::find_if_not(str.cbegin(), str.cend(), allowed_char) != str.cend())
            {
                throw std::invalid_argument(
                    fmt::format("Version contains invalid characters in {}.", str)
                );
            }
        }

        auto parse_common_version(std::string_view str) -> CommonVersion
        {
            assert(!str.empty());
            check_common_version(str);

            static constexpr auto delims_buf = std::array{
                Version::part_delim,
                Version::part_delim_alt,
                Version::part_delim_special,
            };
            static constexpr auto delims = std::string_view{ delims_buf.data(), delims_buf.size() };

            CommonVersion parts = {};
            auto tail = str;
            std::size_t tail_delim_pos = 0;
            while (true)
            {
                tail_delim_pos = tail.find_first_of(delims);
                // `_` is both a delimiter and has sepcial meaning.
                // We need to check if it is at the end and omit it
                if ((tail_delim_pos == tail.size() - 1)
                    && tail[tail_delim_pos] == Version::part_delim_special)
                {
                    tail_delim_pos = std::string_view::npos;
                }
                // One of the part is empty
                if ((tail_delim_pos == 0) || (tail_delim_pos == tail.size() - 1))  // TODO(C++20)
                                                                                   // [[unlikely]]
                {
                    throw std::invalid_argument(fmt::format("Empty part in '{}'.", str));
                }
                parts.push_back(parse_part(tail.substr(0, tail_delim_pos)));
                if (tail_delim_pos == std::string_view::npos)
                {
                    break;
                }
                tail = tail.substr(tail_delim_pos + 1);
            }
            return parts;
        }

        auto parse_trailing_local_version(std::string_view str)
            -> std::pair<std::string_view, CommonVersion>
        {
            const auto delim_pos = str.rfind(Version::local_delim);
            // No local is specified
            if (delim_pos == std::string_view::npos)  // TODO(C++20) [[likely]]
            {
                return { str, {} };
            }
            // local specified but empty
            if (delim_pos + 1 == str.size())
            {
                throw std::invalid_argument(
                    fmt::format("Empty local version delimited by '{}'.", Version::local_delim)
                );
            }
            return { str.substr(0, delim_pos), parse_common_version(str.substr(delim_pos + 1)) };
        }

        auto parse_version(std::string_view str) -> CommonVersion
        {
            if (str.empty())
            {
                throw std::invalid_argument("Empty version.");
            }
            return parse_common_version(str);
        }
    }

    auto Version::parse(std::string_view str) -> Version
    {
        str = strip(str);
        try
        {
            auto [epoch, version_and_local_str] = parse_leading_epoch<std::size_t>(str);
            auto [version_str, local] = parse_trailing_local_version(version_and_local_str);
            auto version = parse_version(version_str);
            return {
                /* .epoch= */ epoch,
                /* .version= */ std::move(version),
                /* .local= */ std::move(local),
            };
        }
        catch (const std::invalid_argument& ia)
        {
            throw std::invalid_argument(fmt::format("Error parsing version '{}'. {}", str, ia.what()));
        }
    }
}
