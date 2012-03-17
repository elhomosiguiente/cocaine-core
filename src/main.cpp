//
// Copyright (C) 2011-2012 Andrey Sibiryov <me@kobology.ru>
//
// Licensed under the BSD 2-Clause License (the "License");
// you may not use this file except in compliance with the License.
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <boost/program_options.hpp>
#include <iostream>

#include "cocaine/config.hpp"

#include "cocaine/context.hpp"
#include "cocaine/core.hpp"
#include "cocaine/overseer.hpp"

#include "cocaine/loggers/syslog.hpp"

#include "cocaine/helpers/pid_file.hpp"

using namespace cocaine;

namespace po = boost::program_options;

int main(int argc, char * argv[]) {
    config_t cfg;

    // Configuration
    // -------------

    cfg.runtime.self = argv[0];

    po::options_description
        hidden_options,
        slave_options,
        general_options("General options"),
        core_options("Core options"),
        storage_options("Storage options"),
        combined_options;
    
    po::positional_options_description positional_options;
    po::variables_map vm;

    hidden_options.add_options()
        ("core:endpoints", po::value< std::vector<std::string> >
            (&cfg.core.endpoints)->composing(),
            "core endpoints for server management");
    
    positional_options.add("core:endpoints", -1);

    slave_options.add_options()
        ("slave", "launch a new slave")
        ("slave:id", po::value<std::string>(&cfg.slave.id))
        ("slave:app:name", po::value<std::string>(&cfg.slave.name));

    general_options.add_options()
        ("help,h", "show this message")
        ("version,v", "show version and build information")
        ("daemonize", "daemonize on start")
        ("pidfile", po::value<std::string>
            ()->default_value("/var/run/cocaine/default.pid"),
            "location of a pid file")
        ("verbose", "produce a lot of output");

    core_options.add_options()
        ("core:modules", po::value<std::string>
            (&cfg.registry.modules)->default_value("/usr/lib/cocaine"),
            "where to load modules from")
        ("core:announce-endpoint", po::value<std::string>
            (&cfg.core.announce_endpoint),
            "multicast endpoint for automatic discovery")
        ("core:announce-interval", po::value<float>
            (&cfg.core.announce_interval)->default_value(5.0f),
            "multicast announce interval for automatic discovery, seconds");

    storage_options.add_options()
        ("storage:driver", po::value<std::string>
            (&cfg.storage.driver)->default_value("files"),
            "storage driver type, built-in storages are: void, files")
        ("storage:uri", po::value<std::string>
            (&cfg.storage.uri)->default_value("/var/lib/cocaine"),
            "storage location, format depends on the storage type");

    combined_options.add(hidden_options)
                    .add(slave_options)
                    .add(general_options)
                    .add(core_options)
                    .add(storage_options);

    try {
        po::store(
            po::command_line_parser(argc, argv).
                options(combined_options).
                positional(positional_options).
                run(),
            vm);
        po::notify(vm);
    } catch(const po::unknown_option& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    } catch(const po::ambiguous_option& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    if(vm.count("help")) {
        std::cout << "Usage: " << argv[0] << " endpoint-list [options]" << std::endl;
        std::cout << general_options << core_options << storage_options;
        return EXIT_SUCCESS;
    }

    if(vm.count("version")) {
        std::cout << "Cocaine " << COCAINE_VERSION << std::endl;
        return EXIT_SUCCESS;
    }
    
    // Startup
    // -------

    cfg.sink.reset(
        new logging::syslog_t(
            "cocaine",
            vm.count("verbose") ? logging::debug : logging::info
        )
    );

    boost::shared_ptr<logging::logger_t> log(
        cfg.sink->get("main")
    );

    if(vm.count("slave")) {
        std::auto_ptr<engine::overseer_t> slave;

        try {
            slave.reset(new engine::overseer_t(cfg));
        } catch(const std::exception& e) {
            log->error("unable to start the slave - %s", e.what());
            return EXIT_FAILURE;
        }

        slave->run();
    } 
 
    else {
        std::auto_ptr<helpers::pid_file_t> pidfile;
        std::auto_ptr<core::core_t> core;

        log->info("starting the core");

        if(vm.count("daemonize")) {
            if(daemon(0, 0) < 0) {
                log->error("daemonization failed");
                return EXIT_FAILURE;
            }

            try {
                pidfile.reset(
                    new helpers::pid_file_t(vm["pidfile"].as<std::string>())
                );
            } catch(const std::runtime_error& e) {
                log->error("%s", e.what());
                return EXIT_FAILURE;
            }
        }
 
        try {
            core.reset(new core::core_t(cfg));
        } catch(const std::exception& e) {
            log->error("unable to start the core - %s", e.what());
            return EXIT_FAILURE;
        }

        core->run();

        log->info("the core has terminated");
    }

    return EXIT_SUCCESS;
}
