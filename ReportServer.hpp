#pragma once

#ifdef WITH_POSTGRESQL

#include "apostol/process_module.hpp"
#include "apostol/bot_session.hpp"
#include "apostol/pg.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace apostol
{

class Application;
class EventLoop;
class Logger;

// --- ReportServer ------------------------------------------------------------
//
// Background process module that executes pre-defined database reports.
//
// Mirrors v1 CReportServer from apostol-crm.
//
// Architecture: logic lives here (ProcessModule), injected into a generic
// ModuleProcess shell via add_custom_process(unique_ptr<ProcessModule>).
//
// Report lifecycle:
//   - Authenticates as "apibot" via OAuth2 client_credentials (BotSession)
//   - Subscribes to PostgreSQL LISTEN "report" channel for immediate dispatch
//   - Polls api.report_ready('enabled') every minute as fallback
//   - For each report in "progress" state: api.execute_report_ready(id)
//   - On success: execute_object_action(id, 'complete')
//   - On cancel:  execute_object_action(id, 'abort')
//   - On error:   execute_object_action(id, 'fail') + set_object_label(id, error)
//
// NOTIFY payload format on the "report" channel:
//   {"session": "...", "id": "<report-uuid>"}
//
// Configuration (in apostol.json):
//   "module": {
//     "ReportServer": {
//       "enable": true,
//       "heartbeat": 60000
//     }
//   }
//
class ReportServer final : public ProcessModule
{
public:
    std::string_view name() const override { return "report-server"; }

    void on_start(EventLoop& loop, Application& app) override;
    void heartbeat(std::chrono::system_clock::time_point now) override;
    void on_stop() override;

private:
    using time_point   = std::chrono::system_clock::time_point;
    using milliseconds = std::chrono::milliseconds;

    // -- State ----------------------------------------------------------------

    PgPool*     pool_{nullptr};
    Logger*     logger_{nullptr};

    std::unique_ptr<BotSession> bot_;

    enum class Status { stopped, running };
    Status status_{Status::stopped};

    struct Report { std::string id; time_point started_at; };
    std::unordered_map<std::string, Report> reports_;

    // Pending report IDs from NOTIFY (processed in heartbeat)
    std::vector<std::string> pending_reports_;

    time_point   next_check_{};
    milliseconds check_interval_{60'000};  // 1 minute

    // -- NOTIFY ---------------------------------------------------------------
    void on_notify(std::string_view payload);
    void process_notify_queue();

    // -- Polling fallback -----------------------------------------------------
    void check_reports();
    void enum_reports(std::vector<PgResult> results);

    // -- Report lifecycle -----------------------------------------------------
    void do_check(const std::string& id);
    void do_start(const std::string& id);
    void do_complete(const std::string& id);
    void do_abort(const std::string& id);
    void do_fail(const std::string& id, const std::string& error);

    void execute_action(const std::string& id, std::string_view action,
                        PgQuery::ResultHandler on_result);
    void delete_report(const std::string& id);
    bool in_progress(const std::string& id) const;

    void on_fatal(const std::string& error);
};

} // namespace apostol

#endif // WITH_POSTGRESQL
