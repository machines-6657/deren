module;

#include <algorithm>
#include <array>
#include <format>
#include <string>

export module deren.utility:data_block;
namespace deren::utility {
    /**
     * @defgroup data_block Fixed-Size Byte Container
     * @ingroup utility
     * @brief struct template creates a sized data type provides auto generated operator==/!= (use std::ranges::equal),
     *     operator<=>(use std::lexicographical_compare_three_way) and hex formatter (.to_hex_string()).
     *     Complete key support: operator< for std::map/std::set keys, hash64() + the nested
     *     hasher functor for std::unordered_map/std::unordered_set keys.
     * @tparam S byte size of the struct
     */
    export template <size_t S>
    struct data_block {
        constexpr static uint32_t size_byte = S;
        std::array<uint8_t, S> data = {}; // NSDMI: default construction zero-initializes

        constexpr static size_t size = S;

        /** @brief zero-initialized block */
        constexpr data_block() noexcept = default;

        /**
         * @brief copy a caller-owned C array of exactly S bytes
         * @param bytes byte array to copy from (compile-time usable, e.g. for static_assert)
         */
        constexpr explicit data_block(uint8_t const (&bytes)[S]) noexcept {
            for (std::size_t i = 0; i < S; ++i) {
                data[i] = bytes[i];
            }
        }

        constexpr bool operator==(data_block<S> const& other) const noexcept {
            return std::ranges::equal(data, other.data);
        }

        constexpr bool operator!=(data_block<S> const& other) const noexcept {
            return !(*this == other);
        }

        constexpr auto operator<=>(data_block<S> const& other) const noexcept {
            return std::lexicographical_compare_three_way(
                data.begin(), data.end(),
                other.data.begin(), other.data.end());
        }

        // Ordering for ordered containers: std::map / std::set keys compare with operator<,
        // which is not synthesized from <=> (C++20 gives us <=> only).
        constexpr bool operator<(data_block<S> const& other) const noexcept {
            return (*this <=> other) < 0;
        }

        /**
         * @brief FNV-1a 64-bit hash of the bytes (unordered-container key support)
         * @return a stable 64-bit fingerprint of the block's contents
         */
        [[nodiscard]] constexpr uint64_t hash64() const noexcept {
            uint64_t hash = 14695981039346656037ull; // FNV offset basis
            for (uint8_t const byte : this->data) {
                hash ^= byte;
                hash *= 1099511628211ull; // FNV prime
            }
            return hash;
        }

        /**
         * @brief hash functor so a data_block can key a std::unordered_map / unordered_set:
         *        std::unordered_map<data_block<N>, V, data_block<N>::hasher>
         */
        struct hasher {
            constexpr size_t operator()(data_block<S> const& block) const noexcept {
                return static_cast<size_t>(block.hash64());
            }
        };

        [[nodiscard]] std::string to_hex_string() const {
            std::string result;
            result.reserve(S * 2);
            std::ranges::for_each(this->data, [&result](auto const& byte) { result += std::format("{:02x}", byte); });
            return result;
        }
    };
} // namespace deren::utility