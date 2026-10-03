module;

#include <cstdint>
#include <cstdio> // deren::utility::print(stderr, ...) below needs the stderr macro (not exportable via modules)
#include <cstring>
// The three platform entry points are declared in a HEADER rather than here: a global module fragment
// may only carry preprocessing directives, and MSVC enforces that (C5202 at /W4, fatal under /WX -
// clang accepts a declaration in this position, which is why it lived here until MSVC was built).
// See utility/platform_functions.hpp for the full note.
#include "platform_functions.hpp"

#include <xxhash.h>

module deren.utility;

std::optional<uint64_t> deren::utility::enable_handle_distribute::distribute() noexcept {
    std::lock_guard guard(this->access_mutex);
    if (!this->recycled_handles.empty()) {
        auto const it = recycled_handles.begin();
        uint64_t handle = *it;
        recycled_handles.erase(it);
        return handle;
    }
    if (this->handle_upper_bound < UINT64_MAX) {
        return this->handle_upper_bound++;
    }
    return std::nullopt;
}

void deren::utility::enable_handle_distribute::recycle(uint64_t const handle) noexcept {
    std::lock_guard guard(this->access_mutex);
    if (handle < this->handle_upper_bound && !this->recycled_handles.contains(handle)) {
        this->recycled_handles.insert(handle);
    }
}

void deren::utility::enable_stack_destruct::register_cleanup(std::function<void()> const& destructor) noexcept {
    std::lock_guard guard(this->access_mutex);
    this->destruct_stack.push(destructor);
}

void deren::utility::enable_stack_destruct::do_cleanup() noexcept {
    // Swap the stack out under the lock, then run the destructors unlocked: a callback may
    // itself call register_cleanup() (it takes the same mutex) and would deadlock otherwise.
    std::stack<destruct_type> pending;
    {
        std::lock_guard guard(this->access_mutex);
        pending.swap(this->destruct_stack); // noexcept
    }
    while (!pending.empty()) {
        pending.top()(); // callbacks run without the lock held
        pending.pop();
    }
}

[[noreturn]] void deren::utility::panic(std::string_view msg, std::source_location source_location) noexcept {
    error("program panic!");

    if (!msg.empty()) {
        error("error info: {}", msg);
    }

    error("occurred at function [{}] line {}", source_location.function_name(), source_location.line());
    error("time point: {:%Y-%m-%d %H:%M:%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));

    // Release writes logs through the async log thread; flush before terminating,
    // otherwise the panic messages above may be lost (std::terminate skips static destructors).
    wait_log_all();

    std::terminate();
}

double deren::utility::timestamp_delta_milliseconds(uint64_t const begin_ticks, uint64_t const end_ticks, uint32_t const valid_bits, float const nanoseconds_per_tick) noexcept {
    if (valid_bits == 0 || nanoseconds_per_tick <= 0.0f) {
        return 0.0;
    }
    // Mask both readings into the counter's width (the driver leaves the bits above
    // timestampValidBits undefined) and take the difference modulo that width, so a wrap inside
    // the measured span comes out right instead of underflowing to a huge value.
    uint64_t const mask = valid_bits >= 64 ? ~uint64_t{0} : ((uint64_t{1} << valid_bits) - 1);
    uint64_t const delta = ((end_ticks - begin_ticks) & mask);
    return static_cast<double>(delta) * static_cast<double>(nanoseconds_per_tick) * 1.0e-6;
}

std::optional<std::vector<uint8_t>> deren::utility::read_binary_to_vector(std::filesystem::path const& path) {
    std::error_code error;
    uintmax_t const file_size = std::filesystem::file_size(path, error);
    if (error) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    // Preallocate based on file size to avoid repeated reallocation while reading
    std::vector<uint8_t> data;
    data.reserve(static_cast<size_t>(file_size));
    data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (file.bad()) {
        return std::nullopt;
    }
    return data;
}

std::optional<std::string> deren::utility::read_binary_to_string(std::filesystem::path const& path) {
    std::error_code error;
    uintmax_t const file_size = std::filesystem::file_size(path, error);
    if (error) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    // Preallocate based on file size to avoid repeated reallocation while reading
    std::string data;
    data.reserve(static_cast<size_t>(file_size));
    data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (file.bad()) {
        return std::nullopt;
    }
    return data;
}

// ---- Async logging (Meyer singleton, internal implementation) ----

namespace {
    // Startup rotation for the Release log file: move the previous session's debug.log content
    // aside to debug.log.old (with a session-end timestamp when the content carries none), then
    // truncate debug.log so the new session starts fresh. Only called in Release builds (NDEBUG).
    [[maybe_unused]] void rotate_previous_log() {
        // Text mode on both sides: the read translates CRLF to LF, the text-mode write
        // translates LF back to CRLF, so line endings stay consistent with debug.log
        std::ifstream current_log("debug.log");
        if (!current_log) {
            return; // no previous log yet
        }
        current_log.seekg(0, std::ios::end);
        if (current_log.tellg() <= 0) {
            return; // empty, nothing to rotate
        }
        current_log.seekg(0, std::ios::beg);

        std::string const content((std::istreambuf_iterator<char>(current_log)), std::istreambuf_iterator<char>());
        current_log.close();

        // Cap debug.log.old: once it exceeds the cap, start it fresh (truncate) instead of
        // appending forever, so the archive stays bounded across many sessions.
        constexpr uintmax_t old_log_cap = 8ull * 1024ull * 1024ull; // 8 MiB
        std::ios::openmode const old_mode = [&] {
            std::error_code ec;
            uintmax_t const size = std::filesystem::file_size("debug.log.old", ec);
            return (!ec && size >= old_log_cap) ? (std::ios::out | std::ios::trunc) : (std::ios::out | std::ios::app);
        }();
        std::ofstream old_log("debug.log.old", old_mode);
        if (!old_log) {
            return;
        }

        // Timestamp the rotated block so sessions are distinguishable in debug.log.old
        if (!content.contains("===== session")) {
            old_log << std::format("===== session ended at {:%Y-%m-%d %H:%M:%S} =====\n",
                                   std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        }

        // Normalize to one blank line after every log line, matching the worker's debug.log
        // format (idempotent: already double-spaced content stays unchanged)
        std::istringstream lines(content);
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.empty()) {
                old_log << line << '\n'
                        << '\n';
            }
        }
        old_log.close();

        // Start the new session with an empty debug.log
        std::ofstream fresh_log("debug.log", std::ios::out | std::ios::trunc);
        fresh_log.close();
    }
} // namespace

deren::utility::log_sink& deren::utility::log_sink::instance() noexcept {
    static log_sink instance;
    return instance;
}

deren::utility::log_sink::log_sink() {
#ifdef NDEBUG
    // Release builds: rotate the previous session's log aside, then append the new session
    rotate_previous_log();
    this->file.open("debug.log", std::ios::out | std::ios::app);
#endif
    this->worker = std::thread([this] { this->worker_loop(); });
}

deren::utility::log_sink::~log_sink() {
    {
        // Stop accepting BEFORE waking the worker: a message enqueued after the worker has drained
        // and exited would leave `pending` above zero forever, and wait_all() (called by panic())
        // would then block the shutdown it is supposed to complete.
        std::lock_guard lock(this->queue_mutex);
        this->accepting = false;
    }
    this->running = false;
    this->queue_cv.notify_all();
    if (this->worker.joinable()) {
        this->worker.join(); // wait for the worker to drain the queue before exiting
    }
#ifdef NDEBUG
    if (this->file.is_open()) {
        this->file.close();
    }
#endif
}

void deren::utility::log_sink::worker_loop() noexcept {
    while (true) {
        std::string message;
        {
            std::unique_lock lock(this->queue_mutex);
            // Keep waiting for messages; drain the queue before exiting
            this->queue_cv.wait(lock, [this] { return !this->running || !this->messages.empty(); });
            if (this->messages.empty()) {
                if (!this->running) {
                    break;
                }
                continue;
            }
            message = std::move(this->messages.front());
            this->messages.pop();
        }
        // Write outside the lock to avoid blocking producers (a blank line follows every
        // message for readability; messages carry no \n)
#ifdef NDEBUG
        if (this->file.is_open()) {
            this->file << message << '\n'
                       << '\n'
                       << std::flush;
        } else {
            deren::utility::println("{}", message); // fall back to the terminal if the file cannot be opened
        }
#else
        deren::utility::println("{}", message);
#endif
        // Decrement pending only after the write finishes so wait_log_all also covers the message being written
        {
            std::lock_guard lock(this->queue_mutex);
            --this->pending;
            if (this->pending == 0) {
                this->drained_cv.notify_all();
            }
        }
    }
}

std::FILE* deren::utility::standard_output() noexcept {
    // The macro lives HERE and not in the interface: `stdout` expands to a call into the C library's FILE table
    // rather than naming an object, so it cannot cross a module boundary - see the declaration's note for the
    // measured reason the interface does not just include <cstdio> and use the macro directly.
    return stdout;
}

void deren::utility::log_sink::write(std::string message) {
    {
        std::lock_guard lock(this->queue_mutex);
        if (!this->accepting) {
            // The sink is shutting down and its worker will not drain anything else: drop the
            // message instead of queueing it for nobody. Late writes are the normal case, not an
            // error - static destructors ordered after this singleton still call log()/error().
            return;
        }
        ++this->pending;
        this->messages.push(std::move(message));
    }
    this->queue_cv.notify_one();
}

void deren::utility::log_sink::wait_all() {
    std::unique_lock lock(this->queue_mutex);
    // Return as soon as the sink stopped accepting: any message still counted in `pending` at that
    // point belongs to a worker that is on its way out, and waiting for it would hang forever.
    this->drained_cv.wait(lock, [this] { return this->pending == 0 || !this->accepting; });
}

void deren::utility::error_message(std::string message) {
#ifdef NDEBUG
    // Release: hand to the log thread (writes to debug.log)
    log_sink::instance().write("[ERROR] " + std::move(message));
#else
    // Debug: print directly to stderr in red, no queueing (error is usually followed by terminate)
    deren::utility::print(stderr, "\x1b[31m[ERROR] {}\x1b[0m\n", message);
#endif
}

uint64_t deren::utility::xxh3_64bits(std::span<uint8_t const> const data_view) {
    return XXH3_64bits(data_view.data(), data_view.size_bytes());
}

void deren::utility::sleep_for_nanoseconds(int64_t const nanoseconds) {
    if (nanoseconds <= 0) {
        return;
    }
    // The platform half does the accurate waiting (see platform_sleep.cpp); the caller may still spin the
    // last fraction of a millisecond if it needs to land exactly on the deadline.
    utility_platform_sleep_ns(nanoseconds);
}

std::filesystem::path deren::utility::executable_directory() {
    // The platform half fills a plain buffer (see platform_path.cpp, which keeps <windows.h> out of
    // the module's global module fragment). A path that does not fit, and a platform with no answer,
    // both come back empty - the caller then falls back to its own lookup rather than failing.
    std::array<char, 32768> buffer = {};
    int32_t const written = utility_platform_executable_directory(buffer.data(), buffer.size());
    if (written <= 0) {
        return {};
    }
    return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(written)));
}

std::optional<std::filesystem::path> deren::utility::ask_open_file(std::string_view const title, std::string_view const filter_patterns) {
    // The platform half fills a plain buffer and reports 1/0/-1 for picked/cancelled/unavailable (see
    // platform_dialog.cpp, which keeps <windows.h> out of the module's global module fragment). Both
    // failures are "no path" here, but they are not the same news: one is the user saying no and the other
    // is the build having no way to ask, so they are logged apart.
    std::array<char, 32768> buffer = {};
    std::string const title_z(title);
    std::string const patterns_z(filter_patterns);
    int32_t const result = utility_platform_ask_open_file(title_z.c_str(), patterns_z.c_str(), buffer.data(), buffer.size());
    if (result > 0) {
        return std::filesystem::path(std::string(buffer.data()));
    }
    if (result == 0) {
        deren::utility::log("file dialog: cancelled by the user");
    } else {
        deren::utility::log("file dialog: no platform backend could ask (headless session, or no zenity/kdialog)");
    }
    return std::nullopt;
}

deren::utility::xxh3_digest deren::utility::xxh3_128bits(std::span<uint8_t const> const data_view) {
    // XXH3_128bits returns a {low64, high64} pair; store its bytes in the digest
    xxh3_digest digest = {};
    XXH128_hash_t const hash = XXH3_128bits(data_view.data(), data_view.size_bytes());
    std::memcpy(digest.data.data(), &hash, sizeof(hash));
    return digest;
}
namespace {
    // CRC32 (PNG chunk checksums) - table generated at compile time
    constexpr std::array<uint32_t, 256> make_crc_table() {
        std::array<uint32_t, 256> table = {};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int32_t k = 0; k < 8; ++k) {
                c = (c & 1u) != 0u ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        return table;
    }
    constexpr std::array<uint32_t, 256> crc_table = make_crc_table();

    constexpr uint32_t crc32_update(uint32_t const crc, uint8_t const byte) {
        return crc_table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
    }

    uint32_t png_crc32(std::string_view const type, std::span<uint8_t const> const payload) {
        uint32_t crc = 0xFFFFFFFFu;
        for (char const character : type) {
            crc = crc32_update(crc, static_cast<uint8_t>(character));
        }
        for (uint8_t const byte : payload) {
            crc = crc32_update(crc, byte);
        }
        return crc ^ 0xFFFFFFFFu;
    }

    /**
     * @brief a sink that forwards to another sink while CRC32-ing what passes through
     * @note this is what lets the (large) IDAT payload stream straight to the file: the chunk's
     *       checksum is known when the payload ends, so nothing has to be buffered for it
     */
    class crc_sink {
    public:
        explicit crc_sink(std::ostream& out) noexcept
            : out(out) {
        }

        void write(char const* const data, std::size_t const size) {
            for (std::size_t i = 0; i < size; ++i) {
                crc = crc32_update(crc, static_cast<uint8_t>(data[i]));
            }
            this->out.write(data, static_cast<std::streamsize>(size)); // ostream counts in streamsize
        }

        [[nodiscard]] uint32_t value() const noexcept {
            return crc ^ 0xFFFFFFFFu;
        }

    private:
        std::ostream& out;
        uint32_t crc = 0xFFFFFFFFu;
    };

    // One PNG chunk: big-endian length, the 4 type bytes, the payload, then the CRC32 over
    // type+payload. The writer is append-only, so the length has to be known up front - which it is.
    std::expected<void, std::string> write_png_chunk(std::ostream& sink, std::string_view const type, std::span<uint8_t const> const payload) {
        return deren::utility::write_binary(sink, deren::utility::be(static_cast<uint32_t>(payload.size())), type, payload, deren::utility::be(png_crc32(type, payload)));
    }
} // namespace

std::expected<void, std::string> deren::utility::write_png(std::filesystem::path const& path, uint32_t const width, uint32_t const height, std::span<uint8_t const> const rgba) {
    std::size_t const expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    if (width == 0 || height == 0 || rgba.size() < expected) {
        return std::unexpected(std::string("write_png: pixel data does not match the dimensions"));
    }

    // raw scanlines: one filter byte (0 = none) followed by the RGBA row. The adler32 of that stream
    // is accumulated in the same pass (the zlib trailer needs it). This is the only buffer the encoder
    // keeps: a stored-deflate block carries its own length, so its length must be known up front.
    std::vector<uint8_t> raw;
    raw.reserve(expected + height);
    uint32_t adler_a = 1;
    uint32_t adler_b = 0;
    auto const adler_update = [&adler_a, &adler_b](uint8_t const byte) {
        adler_a = (adler_a + byte) % 65521u;
        adler_b = (adler_b + adler_a) % 65521u;
    };
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        adler_update(0);
        auto const row = rgba.subspan(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4u, static_cast<std::size_t>(width) * 4u);
        raw.insert(raw.end(), row.begin(), row.end());
        for (uint8_t const byte : row) {
            adler_update(byte);
        }
    }

    // Everything is streamed to the file in binary mode (std::ios::binary matters on Windows: text
    // mode would translate '\n' and corrupt the image). The old encoder held three copies of the
    // frame - scanlines, the zlib stream and the finished PNG - this one holds the scanlines only.
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return std::unexpected(std::format("write_png: cannot open '{}'", path.string()));
    }
    auto const failed = [&path](std::expected<void, std::string> const& result) -> std::expected<void, std::string> {
        return std::unexpected(std::format("write_png: {} ('{}')", result.error(), path.string()));
    };

    constexpr std::array<uint8_t, 8> signature = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    if (auto const written = write_binary(file, signature); !written) {
        return failed(written);
    }

    // IHDR: 13 big-endian + fixed bytes, assembled through the same writer
    std::vector<uint8_t> ihdr;
    ihdr.reserve(13);
    struct vector_sink {
        std::vector<uint8_t>& out;
        void write(char const* data, std::size_t size) {
            out.insert(out.end(), data, data + size);
        }
    };
    vector_sink ihdr_sink{ihdr}; // non-const: the sink's write() mutates it (a const sink fails byte_sink)
    if (auto const written = write_binary(ihdr_sink, be(width), be(height), std::array<uint8_t, 5>{8, 6, 0, 0, 0}); !written) {
        return failed(written);
    }
    if (auto const written = write_png_chunk(file, "IHDR", ihdr); !written) {
        return failed(written);
    }

    // IDAT: a zlib stream of uncompressed (stored) deflate blocks. Length is known before the bytes:
    // 2 header + 5 per block header + the data + the 4-byte adler32 trailer.
    std::size_t const block_count = (raw.size() + 65534u) / 65535u;
    uint32_t const idat_size = static_cast<uint32_t>(2u + block_count * 5u + raw.size() + 4u);
    if (auto const written = write_binary(file, be(idat_size)); !written) {
        return failed(written);
    }
    {
        // the payload goes through a CRC-ing sink so the chunk checksum falls out of the bytes that
        // actually reached the file - no second pass and no buffered copy of the stream. The chunk
        // TYPE must pass through it too: a PNG CRC covers type + payload (the length is the only
        // field outside it).
        crc_sink payload{file};
        if (auto const written = write_binary(payload, std::string_view{"IDAT"}); !written) {
            return failed(written);
        }
        if (auto const written = write_binary(payload,
                                              std::array<uint8_t, 2>{0x78u, 0x01u}); // CM/CINFO + FCHECK
            !written) {
            return failed(written);
        }
        std::size_t offset = 0;
        while (offset < raw.size()) {
            std::size_t const block = std::min<std::size_t>(raw.size() - offset, 65535u);
            bool const last = offset + block >= raw.size();
            // stored-block header: BFINAL/BTYPE byte then LEN and its complement, little-endian
            std::array<uint8_t, 5> header = {
                static_cast<uint8_t>(last ? 1u : 0u),
                static_cast<uint8_t>(block & 0xFFu),
                static_cast<uint8_t>((block >> 8) & 0xFFu),
                static_cast<uint8_t>(~block & 0xFFu),
                static_cast<uint8_t>((~block >> 8) & 0xFFu)};
            if (auto const written = write_binary(payload, header, std::span{raw}.subspan(offset, block)); !written) {
                return failed(written);
            }
            offset += block;
        }
        // zlib's adler32 trailer is big-endian
        if (auto const written = write_binary(payload, be((adler_b << 16) | adler_a)); !written) {
            return failed(written);
        }
        if (auto const written = write_binary(file, be(payload.value())); !written) {
            return failed(written);
        }
    }

    if (auto const written = write_png_chunk(file, "IEND", {}); !written) {
        return failed(written);
    }

    file.flush(); // a failed flush loses the tail: report it instead of returning success
    if (!file) {
        return std::unexpected(std::format("write_png: write failed for '{}'", path.string()));
    }
    return {};
}
