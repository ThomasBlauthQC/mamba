#include <csignal>

#include "output.hpp"
#include "context.hpp"

namespace mamba
{
    Context::Context()
    {
        set_verbosity(0);
        on_ci = (std::getenv("CI") != nullptr);
        if (on_ci)
        {
            no_progress_bars = true;
        }

        std::signal(SIGINT, [](int signum) {
            instance().sig_interrupt = true;
        });
    }

    Context& Context::instance()
    {
        static Context ctx;
        return ctx;
    }

    void Context::set_verbosity(int lvl)
    {
        MessageLogger::global_log_severity() = LogSeverity(lvl);
        this->verbosity = lvl;
    }
}
