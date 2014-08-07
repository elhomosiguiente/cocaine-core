/*
    Copyright (c) 2011-2014 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2011-2014 Other contributors as noted in the AUTHORS file.

    This file is part of Cocaine.

    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "cocaine/context.hpp"
#include "cocaine/defaults.hpp"

#include "cocaine/api/service.hpp"

#include "cocaine/asio/reactor.hpp"
#include "cocaine/asio/resolver.hpp"

#include "cocaine/detail/actor.hpp"
#include "cocaine/detail/engine.hpp"
#include "cocaine/detail/essentials.hpp"

#ifdef COCAINE_ALLOW_RAFT
    #include "cocaine/detail/raft/repository.hpp"
    #include "cocaine/detail/raft/node_service.hpp"
    #include "cocaine/detail/raft/control_service.hpp"
#endif

#include "cocaine/logging.hpp"
#include "cocaine/logging/setup.hpp"
#include "cocaine/memory.hpp"

#include <cstring>

#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>

#include <netdb.h>

#include "rapidjson/reader.h"

#include <blackhole/formatter/json.hpp>
#include <blackhole/frontend/files.hpp>
#include <blackhole/frontend/syslog.hpp>
#include <blackhole/scoped_attributes.hpp>
#include <blackhole/sink/socket.hpp>

using namespace cocaine;
using namespace cocaine::io;

namespace fs = boost::filesystem;

namespace {

struct dynamic_reader_t {
    void
    Null() {
        m_stack.emplace(dynamic_t::null);
    }

    void
    Bool(bool v) {
        m_stack.emplace(v);
    }

    void
    Int(int v) {
        m_stack.emplace(v);
    }

    void
    Uint(unsigned v) {
        m_stack.emplace(dynamic_t::uint_t(v));
    }

    void
    Int64(int64_t v) {
        m_stack.emplace(v);
    }

    void
    Uint64(uint64_t v) {
        m_stack.emplace(dynamic_t::uint_t(v));
    }

    void
    Double(double v) {
        m_stack.emplace(v);
    }

    void
    String(const char* data, size_t size, bool) {
        m_stack.emplace(dynamic_t::string_t(data, size));
    }

    void
    StartObject() {
        // Empty.
    }

    void
    EndObject(size_t size) {
        dynamic_t::object_t object;

        for(size_t i = 0; i < size; ++i) {
            dynamic_t value = std::move(m_stack.top());
            m_stack.pop();

            std::string key = std::move(m_stack.top().as_string());
            m_stack.pop();

            object[key] = std::move(value);
        }

        m_stack.emplace(std::move(object));
    }

    void
    StartArray() {
        // Empty.
    }

    void
    EndArray(size_t size) {
        dynamic_t::array_t array(size);

        for(size_t i = size; i != 0; --i) {
            array[i - 1] = std::move(m_stack.top());
            m_stack.pop();
        }

        m_stack.emplace(std::move(array));
    }

    dynamic_t
    Result() {
        return m_stack.top();
    }

private:
    std::stack<dynamic_t> m_stack;
};

struct rapidjson_ifstream_t {
    rapidjson_ifstream_t(fs::ifstream* backend) :
        m_backend(backend)
    { }

    char
    Peek() const {
        int next = m_backend->peek();

        if(next == std::char_traits<char>::eof()) {
            return '\0';
        } else {
            return next;
        }
    }

    char
    Take() {
        int next = m_backend->get();

        if(next == std::char_traits<char>::eof()) {
            return '\0';
        } else {
            return next;
        }
    }

    size_t
    Tell() const {
        return m_backend->gcount();
    }

    char*
    PutBegin() {
        assert(false);
        return 0;
    }

    void
    Put(char) {
        assert(false);
    }

    size_t
    PutEnd(char*) {
        assert(false);
        return 0;
    }

private:
    fs::ifstream* m_backend;
};

} // namespace

template<>
struct dynamic_converter<cocaine::config_t::component_t> {
    typedef cocaine::config_t::component_t result_type;

    static
    result_type
    convert(const dynamic_t& from) {
        return cocaine::config_t::component_t {
            from.as_object().at("type", "unspecified").as_string(),
            from.as_object().at("args", dynamic_t::object_t())
        };
    }
};

template<>
struct dynamic_converter<cocaine::config_t::logging_t> {
    typedef cocaine::config_t::logging_t result_type;

    static
    result_type
    convert(const dynamic_t& from) {
        result_type component;
        const auto& logging = from.as_object();

        for(auto it = logging.begin(); it != logging.end(); ++it) {
            using namespace blackhole::repository;

            auto object = it->second.as_object();
            auto loggers = object.at("loggers", dynamic_t::array_t());

            config_t::logging_t::logger_t log {
                logmask(object.at("verbosity", defaults::log_verbosity).as_string()),
                object.at("timestamp", defaults::log_timestamp).as_string(),
                config::parser_t<dynamic_t, blackhole::log_config_t>::parse(it->first, loggers)
            };

            component.loggers[it->first] = log;
        }

        return component;
    }

    static inline
    logging::priorities
    logmask(const std::string& verbosity) {
        if(verbosity == "debug") {
            return logging::debug;
        } else if(verbosity == "warning") {
            return logging::warning;
        } else if(verbosity == "error") {
            return logging::error;
        } else {
            return logging::info;
        }
    }
};

config_t::config_t(const std::string& path_) {
    path.configuration = path_;

    const auto configuration_file_status = fs::status(path.configuration);

    if(!fs::exists(configuration_file_status) || !fs::is_regular_file(configuration_file_status)) {
        throw cocaine::error_t("the configuration file path is invalid");
    }

    fs::ifstream stream(path.configuration);

    if(!stream) {
        throw cocaine::error_t("unable to read the configuration file");
    }

    rapidjson::MemoryPoolAllocator<> json_allocator;
    rapidjson::Reader json_reader(&json_allocator);
    rapidjson_ifstream_t json_stream(&stream);

    dynamic_reader_t configuration_constructor;

    if(!json_reader.Parse<rapidjson::kParseDefaultFlags>(json_stream, configuration_constructor)) {
        if(json_reader.HasParseError()) {
            throw cocaine::error_t("the configuration file is corrupted - %s", json_reader.GetParseError());
        } else {
            throw cocaine::error_t("the configuration file is corrupted");
        }
    }

    const dynamic_t root(configuration_constructor.Result());

    // Version validation

    if(root.as_object().at("version", 0).to<unsigned int>() != 3) {
        throw cocaine::error_t("the configuration file version is invalid");
    }

    const auto& path_config = root.as_object().at("paths", dynamic_t::object_t()).as_object();
    const auto& network_config = root.as_object().at("network", dynamic_t::object_t()).as_object();

    // Path configuration

    path.plugins = path_config.at("plugins", defaults::plugins_path).as_string();
    path.runtime = path_config.at("runtime", defaults::runtime_path).as_string();

    const auto runtime_path_status = fs::status(path.runtime);

    if(!fs::exists(runtime_path_status)) {
        throw cocaine::error_t("the %s directory does not exist", path.runtime);
    } else if(!fs::is_directory(runtime_path_status)) {
        throw cocaine::error_t("the %s path is not a directory", path.runtime);
    }

    // Network configuration

    network.pool = network_config.at("pool", boost::thread::hardware_concurrency() * 2).as_uint();

    char hostname[256];

    if(gethostname(hostname, 256) != 0) {
        throw std::system_error(errno, std::system_category(), "unable to determine the hostname");
    }

    addrinfo hints, *result = nullptr;

    std::memset(&hints, 0, sizeof(addrinfo));

    hints.ai_flags = AI_CANONNAME;

    const int rv = getaddrinfo(hostname, nullptr, &hints, &result);

    if(rv != 0) {
        throw std::system_error(rv, gai_category(), "unable to determine the hostname");
    }

    network.hostname = network_config.at("hostname", std::string(result->ai_canonname)).as_string();
    network.endpoint = network_config.at("endpoint", defaults::endpoint).as_string();

    freeaddrinfo(result);

    if(network_config.count("pinned")) {
        network.pinned = network_config.at("pinned").to<decltype(network.pinned)>();
    }

    // Blackhole logging configuration
    logging  = root.as_object().at("logging",  dynamic_t::object_t()).to<config_t::logging_t>();

    // Component configuration
    services = root.as_object().at("services", dynamic_t::object_t()).to<config_t::component_map_t>();
    storages = root.as_object().at("storages", dynamic_t::object_t()).to<config_t::component_map_t>();

#ifdef COCAINE_ALLOW_RAFT
    create_raft_cluster = false;
#endif
}

int
config_t::versions() {
    return COCAINE_VERSION;
}

// Dynamic port mapper

port_mapping_t::port_mapping_t(const config_t& config):
    m_pinned(config.network.pinned)
{
    uint16_t min, max;
    std::tie(min, max) = config.network.shared;

    std::vector<port_t> seed;

    if(min == max == 0 || max <= min) {
        seed.resize(65535);
        std::fill(seed.begin(), seed.end(), 0);
    } else {
        seed.resize(max - min);
        std::iota(seed.begin(), seed.end(), min);
    }

    m_shared = queue_type(seed.begin(), seed.end());
}

port_mapping_t::port_t
port_mapping_t::assign(const std::string& name) {
    if(m_pinned.count(name)) {
        return m_pinned.at(name);
    }

    if(m_shared.empty()) {
        throw cocaine::error_t("no ports left for allocation");
    }

    const auto port = m_shared.top();

    return m_shared.pop(), port;
}

void
port_mapping_t::retain(const std::string& name, port_t port) {
    if(m_pinned.count(name)) {
        return;
    }

    return m_shared.push(port);
}

// Context

context_t::context_t(config_t config_, const std::string& logger_backend):
    m_port_mapping(config_),
    config(config_)
{
    auto& repository = blackhole::repository_t::instance();

    // Available logging sinks.
    typedef boost::mpl::vector<
        blackhole::sink::files_t<>,
        blackhole::sink::syslog_t<logging::priorities>,
        blackhole::sink::socket_t<boost::asio::ip::tcp>,
        blackhole::sink::socket_t<boost::asio::ip::udp>
    > sinks_t;

    // Available logging formatters.
    typedef boost::mpl::vector<
        blackhole::formatter::string_t,
        blackhole::formatter::json_t
    > formatters_t;

    // Register frontends with all combinations of formatters and sinks with the logging repository.
    repository.configure<sinks_t, formatters_t>();

    try {
        using blackhole::keyword::tag::timestamp_t;
        using blackhole::keyword::tag::severity_t;

        // Fetch configuration object.
        auto logger = config.logging.loggers.at(logger_backend);

        // Configure some mappings for timestamps and severity attributes.
        blackhole::mapping::value_t mapper;

        mapper.add<severity_t<logging::priorities>>(&logging::map_severity);
        mapper.add<timestamp_t>(logger.timestamp);

        // Attach them to the logging config.
        auto& frontends = logger.config.frontends;

        for(auto it = frontends.begin(); it != frontends.end(); ++it) {
            it->formatter.mapper = mapper;
        }

        // Register logger configuration with the Blackhole's repository.
        repository.add_config(logger.config);

        typedef blackhole::synchronized<logging::logger_t> logger_type;

        // Try to initialize the logger. If it fails, there's no way to report the failure, except
        // printing it to the standart output.
        m_logger = std::make_unique<logger_type>(repository.create<logging::priorities>(logger_backend));
        m_logger->verbosity(logger.verbosity);
    } catch(const std::out_of_range&) {
        throw cocaine::error_t("the '%s' logger is not configured", logger_backend);
    }

    bootstrap();
}

context_t::context_t(config_t config_, std::unique_ptr<logging::logger_t> logger):
    m_port_mapping(config_),
    config(config_)
{
    // NOTE: The context takes the ownership of the passed logger, so it will become invalid at the
    // calling site after this call.
    m_logger = std::make_unique<blackhole::synchronized<logging::logger_t>>(std::move(*logger));
    logger.reset();

    bootstrap();
}

context_t::~context_t() {
    blackhole::scoped_attributes_t guard(
        *m_logger,
        blackhole::log::attributes_t({ blackhole::keyword::source() = "bootstrap" })
    );

    // COCAINE_LOG_INFO(m_logger, "stopping the synchronization");

    // m_synchronization->shutdown();
    // m_synchronization.reset();

    COCAINE_LOG_INFO(m_logger, "stopping the services");

    // Stop the service from accepting new clients or doing any processing. Pop them from the active
    // service list into this temporary storage, and then destroy them all at once. This is needed
    // because sessions in the execution units might still have references to the services, and their
    // lives have to be extended until those sessions are active.
    std::vector<std::unique_ptr<actor_t>> actors;

    for(auto it = config.services.rbegin(); it != config.services.rend(); ++it) {
        actors.push_back(remove(it->first));
    }

    // There should be no outstanding services left.
    BOOST_ASSERT(m_services->empty());

    COCAINE_LOG_INFO(m_logger, "stopping the execution units");

    m_pool.clear();

    // Kill the services themselves.
    actors.clear();
}

std::unique_ptr<logging::log_t>
context_t::log(const std::string& source) {
    return std::make_unique<logging::log_t>(*m_logger, blackhole::log::attributes_t({
        blackhole::keyword::source() = source
    }));
}

namespace {

struct match {
    template<class T>
    bool
    operator()(const T& service) const {
        return name == service.first;
    }

    const std::string& name;
};

} // namespace

void
context_t::insert(const std::string& name, std::unique_ptr<actor_t> service) {
    blackhole::scoped_attributes_t guard(
        *m_logger,
        blackhole::log::attributes_t({ blackhole::keyword::source() = "bootstrap" })
    );

    {
        auto locked = m_services.synchronize();

        if(std::count_if(locked->begin(), locked->end(), match{name})) {
            throw cocaine::error_t("service '%s' already exists", name);
        }

        // Assign a port to this service. The port might be pinned.
        const auto port = m_port_mapping.assign(name);

        const std::vector<io::tcp::endpoint> endpoints = {{
            boost::asio::ip::address::from_string(config.network.endpoint),
            port
        }};

        service->run(endpoints);

        COCAINE_LOG_INFO(m_logger, "service has been published on %s", service->location().front())(
            "service", name
        );

        locked->emplace_back(name, std::move(service));
    }

    // if(m_synchronization) {
    //     m_synchronization->announce();
    // }
}

auto
context_t::remove(const std::string& name) -> std::unique_ptr<actor_t> {
    blackhole::scoped_attributes_t guard(
        *m_logger,
        blackhole::log::attributes_t({ blackhole::keyword::source() = "bootstrap" })
    );

    std::unique_ptr<actor_t> service;

    {
        auto locked = m_services.synchronize();
        auto it = std::find_if(locked->begin(), locked->end(), match{name});

        if(it == locked->end()) {
            throw cocaine::error_t("service '%s' doesn't exist", name);
        }

        // Release the service's actor ownership.
        service = std::move(it->second);

        const std::vector<io::tcp::endpoint> endpoints = service->location();

        service->terminate();

        COCAINE_LOG_INFO(m_logger, "service has been withdrawn from %s", endpoints.front())(
            "service", name
        );

        m_port_mapping.retain(name, endpoints.front().port());

        locked->erase(it);
    }

    // if(m_synchronization) {
    //     m_synchronization->announce();
    // }

    return service;
}

auto
context_t::locate(const std::string& name) const -> boost::optional<actor_t&> {
    auto locked = m_services.synchronize();
    auto it = std::find_if(locked->begin(), locked->end(), match{name});

    return boost::optional<actor_t&>(it != locked->end(), *it->second);
}

void
context_t::attach(const std::shared_ptr<io::socket<io::tcp>>& ptr, const std::shared_ptr<io::basic_dispatch_t>& dispatch) {
    m_pool[ptr->fd() % m_pool.size()]->attach(ptr, dispatch);
}

void
context_t::bootstrap() {
    blackhole::scoped_attributes_t guard(
        *m_logger,
        blackhole::log::attributes_t({ blackhole::keyword::source() = "bootstrap" })
    );

    COCAINE_LOG_INFO(m_logger, "bootstrapping");

    m_repository = std::make_unique<api::repository_t>(*m_logger);

#ifdef COCAINE_ALLOW_RAFT
    m_raft = std::make_unique<raft::repository_t>(*this);
#endif

    // Load the builtin plugins.
    essentials::initialize(*m_repository);

    // Load the rest of plugins.
    m_repository->load(config.path.plugins);

    COCAINE_LOG_INFO(m_logger, "growing the execution unit pool to %d units", config.network.pool);

    while(m_pool.size() != config.network.pool) {
        m_pool.emplace_back(std::make_unique<execution_unit_t>(*this, "cocaine/io-pool"));
    }

    COCAINE_LOG_INFO(m_logger, "starting %d service(s)", config.services.size());

    for(auto it = config.services.begin(); it != config.services.end(); ++it) {
        auto reactor = std::make_shared<reactor_t>();

        COCAINE_LOG_INFO(m_logger, "starting service")(
            "service", it->first
        );

        try {
            insert(it->first, std::make_unique<actor_t>(*this, reactor, get<api::service_t>(
                it->second.type,
                *this,
                *reactor,
                cocaine::format("service/%s", it->first),
                it->second.args
            )));
        } catch(const std::system_error& e) {
            COCAINE_LOG_ERROR(m_logger, "unable to initialize service: %s", e.code())(
                "service", it->first
            ); throw;
        } catch(const std::exception& e) {
            COCAINE_LOG_ERROR(m_logger, "unable to initialize service: %s", e.what())(
                "service", it->first
            ); throw;
        } catch(...) {
            COCAINE_LOG_ERROR(m_logger, "unable to initialize service")(
                "service", it->first
            ); throw;
        }
    }

    COCAINE_LOG_INFO(m_logger, "bootstrapping has been finished");
}
