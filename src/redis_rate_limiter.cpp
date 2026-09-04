#include "gateway/redis_rate_limiter.hpp"

#include <hiredis.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace gateway {
namespace {

// Token bucket, evaluated entirely inside Redis.
//
//   KEYS[1]  bucket hash key
//   ARGV[1]  capacity in tokens
//   ARGV[2]  refill rate in tokens per second
//   ARGV[3]  key TTL in seconds
//   state    hash {tokens, ts}, ts being Redis TIME in fractional seconds
//   returns  {allowed 0|1, remaining tokens, retry-after milliseconds}
//
// Lua is what makes the read, refill, decision and write one atomic step. Doing
// them as separate commands would let two gateways both observe the last token.
// The clock comes from redis.call('TIME') so gateway hosts need no synchronised
// wall clock; Redis 5+ replicates script effects, so a non-deterministic TIME is
// safe here.
constexpr const char* kTokenBucketScript = R"lua(
local capacity = tonumber(ARGV[1])
local rate = tonumber(ARGV[2])
local ttl = tonumber(ARGV[3])
local time = redis.call('TIME')
local now = tonumber(time[1]) + tonumber(time[2]) / 1000000
local state = redis.call('HMGET', KEYS[1], 'tokens', 'ts')
local tokens = tonumber(state[1])
local ts = tonumber(state[2])
if tokens == nil or ts == nil then
  tokens = capacity
  ts = now
end
if now > ts then
  tokens = math.min(capacity, tokens + (now - ts) * rate)
end
local allowed = 0
local retry = 0
if tokens >= 1 then
  tokens = tokens - 1
  allowed = 1
else
  retry = math.ceil((1 - tokens) / rate * 1000)
end
redis.call('HSET', KEYS[1], 'tokens', tokens, 'ts', now)
redis.call('EXPIRE', KEYS[1], ttl)
return {allowed, math.floor(tokens), retry}
)lua";

struct ReplyDeleter {
    void operator()(redisReply* reply) const noexcept { freeReplyObject(reply); }
};
using ReplyPtr = std::unique_ptr<redisReply, ReplyDeleter>;

bool is_noscript(const redisReply& reply) {
    return reply.type == REDIS_REPLY_ERROR && reply.str != nullptr &&
           std::strstr(reply.str, "NOSCRIPT") != nullptr;
}

}  // namespace

std::string_view redis_token_bucket_script() { return kTokenBucketScript; }

/// One hiredis connection. hiredis contexts are not thread-safe, so a
/// connection is only ever used by the thread that leased it.
class RedisRateLimiter::Connection {
public:
    Connection(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout) {
        const timeval tv{static_cast<time_t>(timeout.count() / 1000),
                         static_cast<suseconds_t>((timeout.count() % 1000) * 1000)};
        context_ = redisConnectWithTimeout(host.c_str(), port, tv);
        if (context_ != nullptr && context_->err == 0) {
            redisSetTimeout(context_, tv);
        }
    }

    ~Connection() {
        if (context_ != nullptr) {
            redisFree(context_);
        }
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    [[nodiscard]] bool ok() const noexcept { return context_ != nullptr && context_->err == 0; }
    [[nodiscard]] redisContext* raw() const noexcept { return context_; }

private:
    redisContext* context_{nullptr};
};

RedisRateLimiter::Lease::Lease(RedisRateLimiter& owner, std::unique_ptr<Connection> connection)
    : owner_(owner), connection_(std::move(connection)) {}

RedisRateLimiter::Lease::~Lease() {
    if (connection_ != nullptr && !discard_ && connection_->ok()) {
        owner_.checkin(std::move(connection_));
    }
}

RedisRateLimiter::RedisRateLimiter(std::string host, std::uint16_t port, std::string key_prefix,
                                   std::uint64_t requests, std::chrono::milliseconds window,
                                   RedisFailurePolicy failure_policy,
                                   std::chrono::milliseconds timeout)
    : host_(std::move(host)),
      port_(port),
      key_prefix_(std::move(key_prefix)),
      requests_(requests == 0 ? 1 : requests),
      window_(window.count() > 0 ? window : std::chrono::milliseconds{1}),
      failure_policy_(failure_policy),
      timeout_(timeout) {}

RedisRateLimiter::~RedisRateLimiter() = default;

std::string RedisRateLimiter::redis_key(std::string_view client_key) const {
    // <prefix>:tb:<requests>-<window ms>:<client>. The algorithm and the policy
    // parameters are part of the key, so a configuration change starts fresh
    // buckets and unrelated policies never share a counter.
    std::string key = key_prefix_;
    key += ":tb:";
    key += std::to_string(requests_);
    key += '-';
    key += std::to_string(window_.count());
    key += ':';
    key.append(client_key);
    return key;
}

RedisRateLimiter::Lease RedisRateLimiter::checkout() {
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        if (!pool_.empty()) {
            std::unique_ptr<Connection> pooled = std::move(pool_.back());
            pool_.pop_back();
            if (pooled->ok()) {
                return Lease(*this, std::move(pooled));
            }
        }
    }
    // Connecting is I/O, so it happens with no lock held.
    auto fresh = std::make_unique<Connection>(host_, port_, timeout_);
    if (!fresh->ok()) {
        return Lease(*this, nullptr);
    }
    return Lease(*this, std::move(fresh));
}

void RedisRateLimiter::checkin(std::unique_ptr<Connection> connection) {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (pool_.size() < kMaxPooledConnections) {
        pool_.push_back(std::move(connection));
    }
}

RateLimitDecision RedisRateLimiter::on_failure() {
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        ++failures_;
    }

    RateLimitDecision decision;
    decision.limit = requests_;
    decision.remaining = 0;
    decision.allowed = failure_policy_ == RedisFailurePolicy::kFailOpen;
    if (!decision.allowed) {
        decision.retry_after = std::chrono::milliseconds{1000};
    }
    return decision;
}

std::uint64_t RedisRateLimiter::failure_count() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return failures_;
}

RateLimitDecision RedisRateLimiter::acquire(std::string_view key, Clock::time_point /*now*/) {
    Lease lease = checkout();
    Connection* connection = lease.get();
    if (connection == nullptr) {
        return on_failure();
    }

    const std::string full_key = redis_key(key);
    const double rate =
        static_cast<double>(requests_) / (static_cast<double>(window_.count()) / 1000.0);
    const std::string capacity_arg = std::to_string(requests_);
    const std::string rate_arg = std::to_string(rate);
    // Long enough for an empty bucket to refill completely, so state outlives
    // the policy window but never lingers.
    const std::string ttl_arg =
        std::to_string(std::max<long long>(1, (window_.count() / 1000) * 2 + 1));

    std::string sha;
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        sha = script_sha_;
    }

    ReplyPtr reply;
    if (!sha.empty()) {
        reply.reset(static_cast<redisReply*>(
            redisCommand(connection->raw(), "EVALSHA %s 1 %s %s %s %s", sha.c_str(),
                         full_key.c_str(), capacity_arg.c_str(), rate_arg.c_str(),
                         ttl_arg.c_str())));
    }

    // No cached script, or Redis restarted and forgot it: send the body once and
    // remember the hash it reports.
    if (reply == nullptr || is_noscript(*reply)) {
        if (reply == nullptr && !sha.empty() && !connection->ok()) {
            lease.discard();
            return on_failure();
        }
        reply.reset(static_cast<redisReply*>(
            redisCommand(connection->raw(), "EVAL %s 1 %s %s %s %s", kTokenBucketScript,
                         full_key.c_str(), capacity_arg.c_str(), rate_arg.c_str(),
                         ttl_arg.c_str())));
        if (reply != nullptr && reply->type == REDIS_REPLY_ARRAY) {
            ReplyPtr loaded(static_cast<redisReply*>(
                redisCommand(connection->raw(), "SCRIPT LOAD %s", kTokenBucketScript)));
            if (loaded != nullptr && loaded->type == REDIS_REPLY_STRING) {
                const std::lock_guard<std::mutex> guard(mutex_);
                script_sha_.assign(loaded->str, static_cast<std::size_t>(loaded->len));
            }
        }
    }

    if (reply == nullptr) {
        lease.discard();
        return on_failure();
    }
    if (reply->type != REDIS_REPLY_ARRAY || reply->elements != 3) {
        return on_failure();
    }
    for (std::size_t i = 0; i < 3; ++i) {
        if (reply->element[i]->type != REDIS_REPLY_INTEGER) {
            return on_failure();
        }
    }

    RateLimitDecision decision;
    decision.allowed = reply->element[0]->integer == 1;
    decision.limit = requests_;
    decision.remaining = static_cast<std::uint64_t>(std::max<long long>(0, reply->element[1]->integer));
    decision.retry_after = std::chrono::milliseconds{std::max<long long>(0, reply->element[2]->integer)};
    return decision;
}

}  // namespace gateway
