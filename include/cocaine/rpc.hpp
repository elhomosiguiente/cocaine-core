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

#ifndef COCAINE_RPC_HPP
#define COCAINE_RPC_HPP

namespace cocaine { namespace engine { namespace rpc {
    enum codes {
        heartbeat,
        terminate,
        invoke,
        push,
        error,
        release
    };
}}}

#endif

