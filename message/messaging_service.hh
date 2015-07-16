/*
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#pragma once

#include "core/reactor.hh"
#include "core/iostream.hh"
#include "core/distributed.hh"
#include "core/print.hh"
#include "core/sstring.hh"
#include "net/api.hh"
#include "util/serialization.hh"
#include "gms/inet_address.hh"
#include "rpc/rpc_types.hh"
#include <unordered_map>
#include "db/config.hh"
#include "frozen_mutation.hh"
#include "query-request.hh"
#include "db/serializer.hh"
#include "mutation_query.hh"

// forward declarations
namespace streaming { namespace messages {
    class stream_init_message;
    class prepare_message;
}}

namespace gms {
    class gossip_digest_syn;
    class gossip_digest_ack;
    class gossip_digest_ack2;
}

class frozen_mutation;

namespace utils {
    class UUID;
}

namespace net {

/* All verb handler identifiers */
enum class messaging_verb : int32_t {
    MUTATION,
    MUTATION_DONE,
    BINARY, // Deprecated
    READ_REPAIR,
    READ,
    READ_DATA,
    READ_MUTATION_DATA, // urchin-only
    READ_DIGEST,
    REQUEST_RESPONSE, // client-initiated reads and writes
    STREAM_INITIATE, // Deprecated
    STREAM_INITIATE_DONE, // Deprecated
    STREAM_REPLY, // Deprecated
    STREAM_REQUEST, // Deprecated
    RANGE_SLICE,
    BOOTSTRAP_TOKEN, // Deprecated
    TREE_REQUEST, // Deprecated
    TREE_RESPONSE, // Deprecated
    JOIN, // Deprecated
    GOSSIP_DIGEST_SYN,
    GOSSIP_DIGEST_ACK,
    GOSSIP_DIGEST_ACK2,
    DEFINITIONS_ANNOUNCE, // Deprecated
    DEFINITIONS_UPDATE,
    TRUNCATE,
    SCHEMA_CHECK,
    INDEX_SCAN, // Deprecated
    REPLICATION_FINISHED,
    INTERNAL_RESPONSE, // responses to internal calls
    COUNTER_MUTATION,
    STREAMING_REPAIR_REQUEST, // Deprecated
    STREAMING_REPAIR_RESPONSE, // Deprecated
    SNAPSHOT, // Similar to nt snapshot
    MIGRATION_REQUEST,
    GOSSIP_SHUTDOWN,
    _TRACE,
    ECHO,
    REPAIR_MESSAGE,
    PAXOS_PREPARE,
    PAXOS_PROPOSE,
    PAXOS_COMMIT,
    PAGED_RANGE,
    UNUSED_1,
    UNUSED_2,
    UNUSED_3,
    // Used by streaming
    STREAM_INIT_MESSAGE,
    PREPARE_MESSAGE,
    STREAM_MUTATION,
    INCOMING_FILE_MESSAGE,
    OUTGOING_FILE_MESSAGE,
    RECEIVED_MESSAGE,
    RETRY_MESSAGE,
    COMPLETE_MESSAGE,
    SESSION_FAILED_MESSAGE,
    LAST,
};

} // namespace net

namespace std {
template <>
class hash<net::messaging_verb> {
public:
    size_t operator()(const net::messaging_verb& x) const {
        return hash<int32_t>()(int32_t(x));
    }
};
} // namespace std

namespace net {

future<> ser_messaging_verb(output_stream<char>& out, messaging_verb& v);
future<> des_messaging_verb(input_stream<char>& in, messaging_verb& v);
future<> ser_sstring(output_stream<char>& out, sstring& v);
future<> des_sstring(input_stream<char>& in, sstring& v);

// NOTE: operator(input_stream<char>&, T&) takes a reference to uninitialized
//       T object and should use placement new in case T is non POD
struct serializer {
    template<typename T>
    inline future<T> read_integral(input_stream<char>& in) {
        static_assert(std::is_integral<T>::value, "T should be integral");

        return in.read_exactly(sizeof(T)).then([] (temporary_buffer<char> buf) {
            if (buf.size() != sizeof(T)) {
                throw rpc::closed_error();
            }
            return make_ready_future<T>(net::ntoh(*unaligned_cast<T*>(buf.get())));
        });
    }

    // Adaptor for writing objects having db::serializer<>
    template<typename Serializable>
    inline future<> write_serializable(output_stream<char>& out, const Serializable& v) {
        db::serializer<Serializable> ser(v);
        bytes b(bytes::initialized_later(), ser.size() + data_output::serialized_size<uint32_t>());
        data_output d_out(b);
        d_out.write<uint32_t>(ser.size());
        ser.write(d_out);
        return out.write(reinterpret_cast<const char*>(b.c_str()), b.size());
    }

    // Adaptor for reading objects having db::serializer<>
    template<typename Serializable>
    inline future<> read_serializable(input_stream<char>& in, Serializable& v) {
        return read_integral<uint32_t>(in).then([&in, &v] (auto sz) mutable {
            return in.read_exactly(sz).then([sz, &v] (temporary_buffer<char> buf) mutable {
                if (buf.size() != sz) {
                    throw rpc::closed_error();
                }
                bytes_view bv(reinterpret_cast<const int8_t*>(buf.get()), sz);
                data_input in(bv);
                new (&v) Serializable(db::serializer<Serializable>::read(in));
            });
        });
    }

    // For integer type
    template<typename T>
    inline auto operator()(output_stream<char>& out, T&& v, std::enable_if_t<std::is_integral<std::remove_reference_t<T>>::value, void*> = nullptr) {
        auto v_ = net::hton(v);
        return out.write(reinterpret_cast<const char*>(&v_), sizeof(T));
    }
    template<typename T>
    inline auto operator()(input_stream<char>& in, T& v, std::enable_if_t<std::is_integral<T>::value, void*> = nullptr) {
        return in.read_exactly(sizeof(v)).then([&v] (temporary_buffer<char> buf) mutable {
            if (buf.size() != sizeof(v)) {
                throw rpc::closed_error();
            }
            v = net::ntoh(*reinterpret_cast<const net::packed<T>*>(buf.get()));
        });
    }

    // For vectors
    template<typename T>
    inline auto operator()(output_stream<char>& out, std::vector<T>& v) {
        return operator()(out, v.size()).then([&out, &v, this] {
            return do_for_each(v.begin(), v.end(), [&out, this] (T& e) {
                return operator()(out, e);
            });
        });
    }
    template<typename T>
    inline auto operator()(input_stream<char>& in, std::vector<T>& v) {
        using size_type = typename  std::vector<T>::size_type;
        return read_integral<size_type>(in).then([&v, &in, this] (size_type c) {
            new (&v) std::vector<T>;
            v.reserve(c);
            union U {
                U(){}
                ~U(){}
                U(U&&) {}
                T v;
            };
            return do_with(U(), [c, &v, &in, this] (U& u) {
                return do_until([c = c] () mutable {return !c--;}, [&v, &in, &u, this] () mutable {
                    return operator()(in, u.v).then([&u, &v] {
                        v.emplace_back(std::move(u.v));
                    });
                });
            });
        });
    }

    // For messaging_verb
    inline auto operator()(output_stream<char>& out, messaging_verb& v) {
        return ser_messaging_verb(out, v);
    }
    inline auto operator()(input_stream<char>& in, messaging_verb& v) {
        return des_messaging_verb(in, v);
    }

    // For sstring
    inline auto operator()(output_stream<char>& out, sstring& v) {
        return ser_sstring(out, v);
    }
    inline auto operator()(input_stream<char>& in, sstring& v) {
        return des_sstring(in, v);
    }

    // For frozen_mutation
    inline auto operator()(output_stream<char>& out, const frozen_mutation& v) {
        return write_serializable(out, v);
    }
    inline auto operator()(output_stream<char>& out, frozen_mutation& v) {
        return write_serializable(out, v);
    }
    inline auto operator()(input_stream<char>& in, frozen_mutation& v) {
        return read_serializable(in, v);
    }

    // For reconcilable_result
    inline auto operator()(output_stream<char>& out, const reconcilable_result& v) {
        return write_serializable(out, v);
    }
    inline auto operator()(output_stream<char>& out, reconcilable_result& v) {
        return write_serializable(out, v);
    }
    inline auto operator()(input_stream<char>& in, reconcilable_result& v) {
        return read_serializable(in, v);
    }

    // For complex types which have serialize()/deserialize(),  e.g. gms::gossip_digest_syn, gms::gossip_digest_ack2
    template<typename T>
    inline auto operator()(output_stream<char>& out, T&& v, std::enable_if_t<!std::is_integral<std::remove_reference_t<T>>::value &&
                                                                             !std::is_enum<std::remove_reference_t<T>>::value, void*> = nullptr) {
        auto sz = serialize_int32_size + v.serialized_size();
        bytes b(bytes::initialized_later(), sz);
        auto _out = b.begin();
        serialize_int32(_out, int32_t(sz - serialize_int32_size));
        v.serialize(_out);
        return out.write(reinterpret_cast<const char*>(b.c_str()), sz);
    }
    template<typename T>
    inline auto operator()(input_stream<char>& in, T& v, std::enable_if_t<!std::is_integral<T>::value &&
                                                                          !std::is_enum<T>::value, void*> = nullptr) {
        return in.read_exactly(serialize_int32_size).then([&in, &v] (temporary_buffer<char> buf) mutable {
            if (buf.size() != serialize_int32_size) {
                throw rpc::closed_error();
            }
            size_t sz = net::ntoh(*reinterpret_cast<const net::packed<int32_t>*>(buf.get()));
            return in.read_exactly(sz).then([sz, &v] (temporary_buffer<char> buf) mutable {
                if (buf.size() != sz) {
                    throw rpc::closed_error();
                }
                bytes_view bv(reinterpret_cast<const int8_t*>(buf.get()), sz);
                new (&v) T(T::deserialize(bv));
                assert(bv.size() == 0);
                return make_ready_future<>();
            });
        });
    }
};

struct shard_id {
    gms::inet_address addr;
    uint32_t cpu_id;
    friend bool operator==(const shard_id& x, const shard_id& y);
    friend bool operator<(const shard_id& x, const shard_id& y);
    friend std::ostream& operator<<(std::ostream& os, const shard_id& x);
    struct hash {
        size_t operator()(const shard_id& id) const;
    };
};

class messaging_service {
public:
    using shard_id = net::shard_id;
    using inet_address = gms::inet_address;
    using UUID = utils::UUID;

    struct rpc_protocol_wrapper;
    struct rpc_protocol_client_wrapper;
    struct rpc_protocol_server_wrapper;

    // FIXME: messaging service versioning
    static constexpr int32_t current_version = 0;

    struct shard_info {
        shard_info(std::unique_ptr<rpc_protocol_client_wrapper>&& client);
        std::unique_ptr<rpc_protocol_client_wrapper> rpc_client;
        rpc::stats get_stats() const;
    };

    void foreach_client(std::function<void(const shard_id& id, const shard_info& info)> f) const;

    void increment_dropped_messages(messaging_verb verb);

    uint64_t get_dropped_messages(messaging_verb verb) const;

    const uint64_t* get_dropped_messages() const;

    int32_t get_raw_version(const gms::inet_address& endpoint) const;

    bool knows_version(const gms::inet_address& endpoint) const;

private:
    static constexpr uint16_t _default_port = 7000;
    gms::inet_address _listen_address;
    uint16_t _port;
    std::unique_ptr<rpc_protocol_wrapper> _rpc;
    std::unique_ptr<rpc_protocol_server_wrapper> _server;
    std::unordered_map<shard_id, shard_info, shard_id::hash> _clients;
    uint64_t _dropped_messages[static_cast<int32_t>(messaging_verb::LAST)] = {};
public:
    messaging_service(gms::inet_address ip = gms::inet_address("0.0.0.0"));
    ~messaging_service();
public:
    uint16_t port();
    gms::inet_address listen_address();
    future<> stop();
    static rpc::no_wait_type no_wait();
public:
    // Wrapper for STREAM_INIT_MESSAGE verb
    void register_stream_init_message(std::function<future<unsigned> (streaming::messages::stream_init_message msg, unsigned src_cpu_id)>&& func);
    future<unsigned> send_stream_init_message(shard_id id, streaming::messages::stream_init_message msg, unsigned src_cpu_id);

    // Wrapper for PREPARE_MESSAGE verb
    void register_prepare_message(std::function<future<streaming::messages::prepare_message> (streaming::messages::prepare_message msg, UUID plan_id,
        inet_address from, inet_address connecting, unsigned dst_cpu_id)>&& func);
    future<streaming::messages::prepare_message> send_prepare_message(shard_id id, streaming::messages::prepare_message msg, UUID plan_id,
        inet_address from, inet_address connecting, unsigned dst_cpu_id);

    // Wrapper for STREAM_MUTATION verb
    void register_stream_mutation(std::function<future<> (frozen_mutation fm, unsigned dst_cpu_id)>&& func);
    future<> send_stream_mutation(shard_id id, frozen_mutation fm, unsigned dst_cpu_id);

    // Wrapper for ECHO verb
    void register_echo(std::function<future<> ()>&& func);
    future<> send_echo(shard_id id);

    // Wrapper for GOSSIP_SHUTDOWN
    void register_gossip_shutdown(std::function<rpc::no_wait_type (inet_address from)>&& func);
    future<> send_gossip_shutdown(shard_id id, inet_address from);

    // Wrapper for GOSSIP_DIGEST_SYN
    void register_gossip_digest_syn(std::function<future<gms::gossip_digest_ack> (gms::gossip_digest_syn)>&& func);
    future<gms::gossip_digest_ack> send_gossip_digest_syn(shard_id id, gms::gossip_digest_syn msg);

    // Wrapper for GOSSIP_DIGEST_ACK2
    void register_gossip_digest_ack2(std::function<rpc::no_wait_type (gms::gossip_digest_ack2)>&& func);
    future<> send_gossip_digest_ack2(shard_id id, gms::gossip_digest_ack2 msg);

    // Wrapper for DEFINITIONS_UPDATE
    void register_definitions_update(std::function<rpc::no_wait_type (std::vector<frozen_mutation> fm)>&& func);
    future<> send_definitions_update(shard_id id, std::vector<frozen_mutation> fm);

    // FIXME: response_id_type is an alias in service::storage_proxy::response_id_type
    using response_id_type = uint64_t;
    // Wrapper for MUTATION
    void register_mutation(std::function<rpc::no_wait_type (frozen_mutation fm, std::vector<inet_address> forward,
        inet_address reply_to, unsigned shard, response_id_type response_id)>&& func);
    future<> send_mutation(shard_id id, const frozen_mutation& fm, std::vector<inet_address> forward,
        inet_address reply_to, unsigned shard, response_id_type response_id);

    // Wrapper for MUTATION_DONE
    void register_mutation_done(std::function<rpc::no_wait_type (rpc::client_info cinfo, unsigned shard, response_id_type response_id)>&& func);
    future<> send_mutation_done(shard_id id, unsigned shard, response_id_type response_id);

    // Wrapper for READ_DATA
    // Note: WTH is future<foreign_ptr<lw_shared_ptr<query::result>>
    void register_read_data(std::function<future<foreign_ptr<lw_shared_ptr<query::result>>> (query::read_command cmd, query::partition_range pr)>&& func);
    future<query::result> send_read_data(shard_id id, query::read_command& cmd, query::partition_range& pr);

    // Wrapper for READ_MUTATION_DATA
    void register_read_mutation_data(std::function<future<foreign_ptr<lw_shared_ptr<reconcilable_result>>> (query::read_command cmd, query::partition_range pr)>&& func);
    future<reconcilable_result> send_read_mutation_data(shard_id id, query::read_command& cmd, query::partition_range& pr);

    // Wrapper for READ_DIGEST
    void register_read_digest(std::function<future<query::result_digest> (query::read_command cmd, query::partition_range pr)>&& func);
    future<query::result_digest> send_read_digest(shard_id id, query::read_command& cmd, query::partition_range& pr);

public:
    // Return rpc::protocol::client for a shard which is a ip + cpuid pair.
    rpc_protocol_client_wrapper& get_rpc_client(shard_id id);
    void remove_rpc_client(shard_id id);
    std::unique_ptr<rpc_protocol_wrapper>& rpc();
};

extern distributed<messaging_service> _the_messaging_service;

inline distributed<messaging_service>& get_messaging_service() {
    return _the_messaging_service;
}

inline messaging_service& get_local_messaging_service() {
    return _the_messaging_service.local();
}

future<> init_messaging_service(sstring listen_address, db::config::seed_provider_type seed_provider);
} // namespace net
