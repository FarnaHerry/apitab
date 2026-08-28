module;
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/dispatch.hpp>
#include <asio/defer.hpp>
#include <asio/steady_timer.hpp>
#include <asio/thread_pool.hpp>
#include <asio/strand.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/buffer.hpp>
#include <asio/awaitable.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/execution_context.hpp>
#include <asio/any_io_executor.hpp>
#include <asio/system_executor.hpp>
#include <asio/system_context.hpp>
#include <asio/associated_executor.hpp>
#include <asio/associated_allocator.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/error_code.hpp>
#include <asio/detached.hpp>
#include <asio/use_future.hpp>
#include <asio/deferred.hpp>
#include <asio/redirect_error.hpp>
#include <asio/bind_executor.hpp>
#include <asio/signal_set.hpp>
#include <asio/system_timer.hpp>
#include <asio/bind_allocator.hpp>
#include <asio/append.hpp>
#include <asio/prepend.hpp>
#include <asio/consign.hpp>
#include <asio/as_tuple.hpp>
#include <asio/socket_base.hpp>
#include <asio/connect.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <asio/read_until.hpp>
#include <asio/ip/udp.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/address_v6.hpp>
#include <asio/error.hpp>
// File I/O exists only where Asio has a backend for it: IOCP on Windows, or
// io_uring on Linux (asio/detail/config.hpp, "// Files."). ASIO_HAS_FILE is
// Asio's own answer to that question, so the guard here is the library's, not
// a platform list this descriptor would have to keep in sync.
#if defined(ASIO_HAS_FILE)
#include <asio/file_base.hpp>
#include <asio/stream_file.hpp>
#include <asio/random_access_file.hpp>
#endif
#include <asio/experimental/promise.hpp>
#include <asio/experimental/channel_error.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/experimental/concurrent_channel.hpp>
#include <asio/experimental/use_promise.hpp>
#include <asio/experimental/parallel_group.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#ifdef APITAB_ASIO_SSL
#include <asio/ssl.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/ssl/error.hpp>
#endif

export module asio;

export namespace asio::detail {
using ::std::chrono::operator==;
using ::std::chrono::operator<;
using ::std::chrono::operator>=;
using ::std::chrono::operator+;
using ::std::chrono::operator-;
using ::std::coroutine_traits;
}

export namespace asio::error {
using ::asio::error::make_error_code;
// The four error enums and their enumerators. Exporting `operation_aborted`
// alone made every OTHER condition unreachable through the module — a consumer
// that wants to tell a connection refusal from a DNS failure had no name to
// compare against. `using enum` keeps that from becoming a hand-maintained
// list of ~40 enumerators that drifts on the next Asio release.
using ::asio::error::basic_errors;
using ::asio::error::netdb_errors;
using ::asio::error::addrinfo_errors;
using ::asio::error::misc_errors;
using enum ::asio::error::basic_errors;
using enum ::asio::error::netdb_errors;
using enum ::asio::error::addrinfo_errors;
using enum ::asio::error::misc_errors;
}

export namespace asio {
using ::asio::io_context;
using ::asio::post;
using ::asio::make_work_guard;
using ::asio::executor_work_guard;
using ::asio::dispatch;
using ::asio::defer;
using ::asio::steady_timer;
using ::asio::thread_pool;
using ::asio::make_strand;
using ::asio::mutable_buffer;
using ::asio::const_buffer;
using ::asio::buffer;
using ::asio::awaitable;
using ::asio::use_awaitable;
using ::asio::co_spawn;
using ::asio::cancellation_signal;
using ::asio::cancellation_type;
using ::asio::bind_cancellation_slot;
using ::asio::execution_context;
using ::asio::any_io_executor;
using ::asio::system_executor;
using ::asio::system_context;
using ::asio::associated_executor;
using ::asio::associated_allocator;
using ::asio::associated_cancellation_slot;
using ::asio::error_code;
using ::asio::detached;
using ::asio::detached_t;
using ::asio::use_future;
using ::asio::deferred;
using ::asio::deferred_t;
using ::asio::redirect_error;
using ::asio::bind_executor;
using ::asio::signal_set;
using ::asio::system_timer;
using ::asio::bind_allocator;
using ::asio::append;
using ::asio::prepend;
using ::asio::consign;
using ::asio::as_tuple;
using ::asio::socket_base;
using ::asio::connect;
using ::asio::async_connect;
using ::asio::async_read;
using ::asio::async_write;
using ::asio::read;
using ::asio::write;
using ::asio::read_until;
}

#if defined(ASIO_HAS_FILE)
export namespace asio {
using ::asio::file_base;
using ::asio::basic_file;
using ::asio::basic_stream_file;
using ::asio::basic_random_access_file;
using ::asio::stream_file;
using ::asio::random_access_file;
}
#endif

export namespace asio::experimental {
using ::asio::experimental::channel;
using ::asio::experimental::concurrent_channel;
using ::asio::experimental::use_promise;
}

export namespace asio::experimental::error {
using ::asio::experimental::error::make_error_code;
}

export namespace asio::ip {
using ::asio::ip::tcp;
using ::asio::ip::udp;
using ::asio::ip::address;
using ::asio::ip::address_v4;
using ::asio::ip::address_v6;
using ::asio::ip::make_address;
using ::asio::ip::make_address_v4;
using ::asio::ip::make_address_v6;
}

export namespace asio::this_coro {
using ::asio::this_coro::executor;
using ::asio::this_coro::cancellation_state;
using ::asio::this_coro::throw_if_cancelled;
using ::asio::this_coro::reset_cancellation_state;
}

#ifdef APITAB_ASIO_SSL
export namespace asio::ssl {
using ::asio::ssl::context;
using ::asio::ssl::context_base;
using ::asio::ssl::stream;
using ::asio::ssl::stream_base;
using ::asio::ssl::verify_context;
using ::asio::ssl::verify_mode;
using ::asio::ssl::host_name_verification;
}

export namespace asio::ssl::error {
using ::asio::ssl::error::stream_errors;
using ::asio::ssl::error::make_error_code;
// stream_category is static const ref (internal linkage) — can't export.
}
#endif
