#include <sstream>

#include <boost/bind.hpp>
#include <boost/tuple/tuple.hpp>
#include <msgpack.hpp>

#include "engine.hpp"
#include "future.hpp"
#include "schedulers.hpp"

using namespace yappi::core;
using namespace yappi::engine;
using namespace yappi::plugin;
using namespace yappi::persistance;
using namespace yappi::helpers;

engine_t::engine_t(zmq::context_t& context, registry_t& registry, storage_t& storage, const std::string& target):
    m_context(context),
    m_registry(registry),
    m_storage(storage),
    m_target(target)
{
    syslog(LOG_DEBUG, "engine: starting for %s", m_target.c_str());
}

engine_t::~engine_t() {
    syslog(LOG_DEBUG, "engine: terminating for %s", m_target.c_str()); 
    m_threads.clear();
}

engine_t::thread_t::thread_t(zmq::context_t& context, std::auto_ptr<source_t> source, storage_t& storage): 
    m_context(context),
    m_pipe(m_context, ZMQ_PUSH),
    m_source(source),
    m_storage(storage)
{
    syslog(LOG_DEBUG, "threading: starting thread %s", m_uuid.get().c_str());

    m_pipe.bind("inproc://" + m_uuid.get());
    
    if(pthread_create(&m_thread, NULL, &bootstrap, this) == EAGAIN) {
        throw std::runtime_error("system thread limit exceeded");
    }
}

engine_t::thread_t::~thread_t() {
    syslog(LOG_DEBUG, "threading: terminating thread %s", m_uuid.get().c_str());
    
    Json::Value message;
    message["command"] = "terminate";
    
    send(message);
    pthread_join(m_thread, NULL); 
}

void* engine_t::thread_t::bootstrap(void* args) {
    thread_t* thread = static_cast<thread_t*>(args);

    overseer_t overseer(thread->m_context, *thread->m_source,
        thread->m_storage, thread->m_uuid);

    overseer.run();

    return NULL;
}

void engine_t::push(future_t* future, const Json::Value& args) {
    Json::Value message, response;
    std::string thread_id = "default";
    
    thread_map_t::iterator it = m_threads.find(thread_id);

    if(it == m_threads.end()) {
        std::auto_ptr<source_t> source;
        std::auto_ptr<thread_t> thread;

        try {
            source.reset(m_registry.instantiate(m_target));
            thread.reset(new thread_t(m_context, source, m_storage));
            boost::tie(it, boost::tuples::ignore) = m_threads.insert(thread_id, thread);
        } catch(const std::runtime_error& e) {
            response["error"] = e.what();
            future->fulfill(m_target, response);
            return;
        }
    }
        
    message["command"] = args.get("type", "simple");
    message["future"] = future->serialize();
    message["args"] = args;
    
    it->second->send(message);
}

void engine_t::drop(future_t* future, const Json::Value& args) {
    Json::Value message, response;
    std::string thread_id = "default";

    thread_map_t::iterator it = m_threads.find(thread_id);

    if(it == m_threads.end()) {
        response["error"] = "not found";
        future->fulfill(m_target, response);
    } else {
        message["command"] = "stop";
        message["future"] = future->serialize();
        message["args"] = args;
    
        it->second->send(message);
    }
}

void engine_t::kill(const std::string& thread_id) {
    thread_map_t::iterator it = m_threads.find(thread_id);

    if(it == m_threads.end()) {
        syslog(LOG_DEBUG, "engine: found an orphan - thread %s", thread_id.c_str());
        return;
    }

    m_threads.erase(it);
}

overseer_t::overseer_t(zmq::context_t& context, source_t& source, storage_t& storage, const auto_uuid_t& uuid):
    m_context(context),
    m_pipe(m_context, ZMQ_PULL),
    m_futures(m_context, ZMQ_PUSH),
    m_reaper(m_context, ZMQ_PUSH),
    m_source(source),
    m_storage(storage),
    m_loop(),
    m_io(m_loop),
    m_suicide(m_loop),
    m_cleanup(m_loop),
    m_cached(false)
{
    // Cache cleanup watcher
    m_cleanup.set(this);
    m_cleanup.start();

    // Connect to the engine's controlling socket
    // and set the socket watcher
    m_pipe.connect("inproc://" + uuid.get());
    m_io.set(this);
    m_io.start(m_pipe.fd(), EV_READ);

    // [CONFIG] Initializing suicide timer
    m_suicide.set(this);
    m_suicide.start(600.);

    // Connecting to the core's future sink
    m_futures.connect("inproc://futures");

    // Connecting to the core's reaper sink
    m_reaper.connect("inproc://reaper");

    // [CONFIG] Set timer compression threshold
    // m_loop.set_timeout_collect_interval(0.500);

    // Signal a false event, in case the core 
    // has managed to send something already
    m_loop.feed_fd_event(m_pipe.fd(), EV_READ);
}

void overseer_t::run() {
    m_loop.loop();
}

void overseer_t::operator()(ev::io& w, int revents) {
    std::string command;
    
    while(m_pipe.pending()) {
        Json::Value message;
        
        m_pipe.recv(message);
        command = message["command"].asString();
        
        if(command == "auto") {
            schedule<auto_scheduler_t>(message);
        } else if(command == "manual") {
            schedule<manual_scheduler_t>(message);
        } else if(command == "once") {
            once(message);
        } else if(command == "stop") {
            stop(message);
        } else if(command == "terminate") {
            terminate();
            break;
        }
    }
}
 
void overseer_t::operator()(ev::timer& w, int revents) {
    suicide();
}

void overseer_t::operator()(ev::prepare& w, int revents) {
    m_cache.clear();
    m_cached = false;
}

source_t::dict_t overseer_t::fetch() {
    if(!m_cached) {
        try {
            m_cache = m_source.fetch();
            m_cached = true;
        } catch(const std::exception& e) {
            syslog(LOG_NOTICE, "overseer: exception in %s - %s",
                m_source.uri().c_str(), e.what());
            suicide();
        }
    }

    return m_cache;
}

template<class SchedulerType>
void overseer_t::schedule(const Json::Value& message) {
    Json::Value result;
    std::string token = message["future"]["token"].asString();
    std::string key;

    std::auto_ptr<SchedulerType> scheduler;

    try {
        scheduler.reset(new SchedulerType(m_context, m_source, *this, message["args"]));
        key = scheduler->key();
    } catch(const std::runtime_error& e) {
        result["error"] = e.what();
        respond(message["future"], result);
        return;
    }
    
    // Scheduling
    if(m_slaves.find(key) == m_slaves.end()) {
        scheduler->start();
        m_slaves.insert(key, scheduler);

        if(m_suicide.is_active()) {
            syslog(LOG_DEBUG, "overseer: suicide timer stopped for %s", m_source.uri().c_str());
            m_suicide.stop();
        }
    }

    // ACL
    subscription_map_t::const_iterator begin, end;
    boost::tie(begin, end) = m_subscriptions.equal_range(token);
    subscription_map_t::value_type subscription = std::make_pair(token, key);
    std::equal_to<subscription_map_t::value_type> equality;

    if(std::find_if(begin, end, boost::bind(equality, subscription, _1)) == end) {
        syslog(LOG_DEBUG, "overseer: subscribing %s to %s", token.c_str(),
            m_source.uri().c_str());
        m_subscriptions.insert(subscription);
    }
    
    // Persistance
    if(!message["args"].get("transient", false).asBool()) {
        std::string object_id = m_digest.get(key + token);

        if(!m_storage.exists(object_id)) {
            Json::Value object;
            
            object["url"] = m_source.uri();
            object["args"] = message["args"];
            object["token"] = message["future"]["token"];
            
            m_storage.put(object_id, object);
        }
    }

    // Report to the core
    result["key"] = key;
    respond(message["future"], result);
}

void overseer_t::once(const Json::Value& message) {
    Json::Value result;
    source_t::dict_t dict = fetch();

    for(source_t::dict_t::const_iterator it = dict.begin(); it != dict.end(); ++it) {
        result[it->first] = it->second;
    }

    // Report to the core
    respond(message["future"], result);

    // Rearm the stall timer if it's active
    if(m_suicide.is_active()) {
        syslog(LOG_DEBUG, "overseer: suicide timer rearmed for %s", m_source.uri().c_str());
        m_suicide.stop();
        m_suicide.start(600.); // [CONFIG]
    }
}

void overseer_t::stop(const Json::Value& message) {

}

void overseer_t::terminate() {
    syslog(LOG_INFO, "overseer: stopping for %s", m_source.uri().c_str());

    // Kill everything
    m_slaves.clear();
    m_suicide.stop();
    m_io.stop();
    m_cleanup.stop();
} 

void overseer_t::suicide() {
    Json::Value message;

    message["engine"] = m_source.uri();
    message["thread"] = "default";

    // This is a suicide ;(
    m_reaper.send(message);    
}

scheduler_base_t::scheduler_base_t(zmq::context_t& context, source_t& source, overseer_t& overseer):
    m_uplink(context, ZMQ_PUSH),
    m_source(source),
    m_overseer(overseer)
{}

scheduler_base_t::~scheduler_base_t() {
    if(m_watcher->is_active()) {
        m_watcher->stop();
    }
}

void scheduler_base_t::start() {
    m_uplink.connect("inproc://events");
        
    m_watcher.reset(new ev::periodic(m_overseer.binding()));
    m_watcher->set<scheduler_base_t, &scheduler_base_t::publish>(this);
    ev_periodic_set(static_cast<ev_periodic*>(m_watcher.get()), 0, 0, thunk);
    m_watcher->start();
}

void scheduler_base_t::publish(ev::periodic& w, int revents) {
    source_t::dict_t dict = m_overseer.fetch();

    // Do nothing if plugin has returned an empty dict
    if(dict.size() == 0) {
        return;
    }

    zmq::message_t message(m_key.length());
    memcpy(message.data(), m_key.data(), m_key.length());
    m_uplink.send(message, ZMQ_SNDMORE);

    // Serialize the dict
    msgpack::sbuffer buffer;
    msgpack::pack(buffer, dict);

    // And send it
    message.rebuild(buffer.size());
    memcpy(message.data(), buffer.data(), buffer.size());
    m_uplink.send(message);
}
