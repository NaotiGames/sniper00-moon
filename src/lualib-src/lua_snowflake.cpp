#include "lua.hpp"
#include <iostream>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <memory>
#include <inttypes.h>


#define METANAME "lsnowflake"

const int DATACENTER_ID_BITS = 5;
const int WORKER_ID_BITS = 5;
const int SEQUENCE_BITS = 12;

class Snowflake {

public:
    Snowflake(int datacenter_id, int worker_id)
        : datacenter_id_(datacenter_id)
        , worker_id_(worker_id)
        , sequence_(0)
        , last_timestamp_(0)
    {
        if (datacenter_id < 0 || datacenter_id >= (1 << DATACENTER_ID_BITS)) {
            throw std::invalid_argument("Datacenter ID out of range");
        }

        if (worker_id < 0 || worker_id >= (1 << WORKER_ID_BITS)) {
            throw std::invalid_argument("Worker ID out of range");
        }
    }

    uint64_t next()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        uint64_t timestamp = current_time();

        if (timestamp < last_timestamp_)
        {
            throw std::runtime_error("Clock moved backwards. Refusing to generate id");
        }

        if (timestamp == last_timestamp_)
        {
            sequence_ = (sequence_ + 1) & sequence_mask;
            if (sequence_ == 0) {
                timestamp = wait_for_next_millis(last_timestamp_);
            }
        }
        else
        {
            sequence_ = 0;
        }

        last_timestamp_ = timestamp;

        return ((timestamp - epoch) << timestamp_left_shift)
            | (datacenter_id_ << datacenter_id_shift)
            | (worker_id_ << worker_id_shift)
            | sequence_;
    }

private:
    const int max_datacenter_id = (1 << DATACENTER_ID_BITS) - 1;
    const int max_worker_id = (1 << WORKER_ID_BITS) - 1;
    const int sequence_mask = (1 << SEQUENCE_BITS) - 1;

    const int datacenter_id_shift = SEQUENCE_BITS + WORKER_ID_BITS;
    const int worker_id_shift = SEQUENCE_BITS;
    const int timestamp_left_shift = SEQUENCE_BITS + WORKER_ID_BITS + DATACENTER_ID_BITS;

    const uint64_t epoch = 1577836800000; // 2020-01-01 00:00:00 UTC in milliseconds

    int datacenter_id_;
    int worker_id_;
    uint64_t sequence_;
    uint64_t last_timestamp_;
    std::mutex mutex_;

    uint64_t current_time()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    uint64_t wait_for_next_millis(uint64_t last_timestamp)
    {
        uint64_t timestamp = current_time();
        while (timestamp <= last_timestamp) {
            timestamp = current_time();
        }
        return timestamp;
    }
};

struct snowflake_state {
public:
    void init(int datacenter_id, int worker_id)
    {
        if (snowflake)
        {
            snowflake.reset();
            snowflake = nullptr;
        }

        if (!snowflake)
        {
            snowflake = std::make_shared<Snowflake>(datacenter_id, worker_id);
        }
    }

    bool next(uint64_t& id)
    {
        if (snowflake)
        {
            id = snowflake->next();

            return true;
        }

        return false;
    }

private:
    std::shared_ptr<Snowflake> snowflake;
};

snowflake_state g_snowflake;

static int linit(lua_State* L) {
    int datacenter_id = (int)luaL_checkinteger(L, 1);
    int worker_id = (int)luaL_checkinteger(L, 2);

    luaL_argcheck(L, (datacenter_id > 0 && datacenter_id < (1 << DATACENTER_ID_BITS)), 1, "datacenter_id out off limit");
    luaL_argcheck(L, (worker_id > 0 && worker_id < (1 << WORKER_ID_BITS)), 2, "worker_id out off limit");

    g_snowflake.init(datacenter_id, worker_id);

    return 0;
}

static int lnext(lua_State* L) {
    uint64_t id = 0;
    if (!g_snowflake.next(id))
    {
        return luaL_error(L, "not init");
    }

    char s[64];
    #if defined(_WIN32)
    sprintf_s(s, sizeof(s), "%" PRIu64, id);
	#else
    snprintf(s, sizeof(s), "%" PRIu64, id);
	#endif
    lua_pushstring(L, s);
    return 1;
}

extern "C"
{
int LUAMOD_API luaopen_snowflake(lua_State* L) {
    luaL_Reg l[] = {
        { "init", linit },
        { "next", lnext },
        { nullptr, nullptr }
    };
    luaL_checkversion(L);
    luaL_newlib(L, l);
    return 1;
}
}
