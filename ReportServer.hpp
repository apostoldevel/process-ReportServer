#pragma once

#ifdef WITH_POSTGRESQL

#include "apostol/process_module.hpp"
#include "apostol/bot_session.hpp"
#include "apostol/pg.hpp"

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Path `single` of the notify channel `report` needs the db-platform subscriber;
// a libapostol without it builds the module with the path `legacy` only.
#if defined(WITH_DB_PLATFORM) && __has_include("apostol/notify_subscriber.hpp")
#include "apostol/notify_subscriber.hpp"
#define REPORTSERVER_NOTIFY 1
#endif

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
// Two paths, by the mode of the notify channel `report` (notify.send decides
// which one a report takes; both may be live during a switch):
//
// Path `legacy` — the lifecycle above. The poll waits for the probe of the
//   notify module, then asks the channel's mode in the same statement and
//   finds nothing once the channel is `single`.
//
// Path `single` (T1103, E4 of T996) — a row of notify.queue, handed to one
// ReportServer:
//   heartbeat()  → until the channel is read `single`: notify.channel_policy;
//                  then a NotifySubscriber of `report` for the life of the
//                  process (rows are brought to their end in any mode)
//   a row        → waits here while max_in_flight reports are out; then
//                  get_report_ready: `progress` → the build; `canceled` →
//                  `abort`; anything else — confirmed, nothing to do
//   the build    → notify.hold (the row first: no deadlock with the sweep,
//                  T1147), statement limits below the lease,
//                  execute_report_ready and notify.ack in one transaction: a build longer than the lease
//                  fails as an error, never rolled back unseen by the ack of a
//                  token that is not current any more
//   build failed → by the state: `canceled` → `abort`, `progress` → `fail`
//                  and the label, anything else — confirmed; each with the
//                  hold and the ack
//   no poll      → a lost wake-up is the subscriber's sweep's to repeat. A
//                  cancel reaches a build here only when it ends (complete()
//                  gives no query id to cancel by — T1148): the routines'
//                  `complete` is refused in `canceled`, and the report is
//                  aborted
//
// The stop (the owner's В4 — finish what is started): on_stopping takes
// nothing new and gives back what is not being built yet; a build sent with
// its ack is the pool's to wait for.
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
    void on_stopping() override;
    bool busy() const override;
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

    struct Report {
        std::string id;
        std::string session;      // the scope this report was found in; act in it
        time_point  started_at;
        uint64_t    query_id{0};  // PgPool query handle for cancel
#ifdef REPORTSERVER_NOTIFY
        std::optional<db_platform::NotifyRow> row;   // path single: the delivery it came with
        bool        with_subscriber{false};          // path single: no complete() yet
        bool        building{false};                 // path single: the build is sent
#endif
    };

    std::unordered_map<std::string, Report> reports_;

    // Pending reports from NOTIFY (processed in heartbeat). The session comes with
    // the notification — pg_notify('report', …) carries current_session() — so the
    // scope is known without guessing which of ours it belongs to.
    struct Pending {
        std::string id;
        std::string session;
    };
    std::vector<Pending> pending_reports_;

    time_point   next_check_{};
    milliseconds check_interval_{60'000};   // 1 minute
    milliseconds report_timeout_{300'000};  // 5 minutes — stale report cleanup
    std::size_t  max_in_flight_{5};
    std::size_t  max_pending_{1000};

    // -- NOTIFY ---------------------------------------------------------------
    void on_notify(std::string_view payload);
    void process_notify_queue();

    // -- Polling fallback -----------------------------------------------------
    void check_reports();
    void enum_reports(const std::string& session, std::vector<PgResult> results);

    // -- Report lifecycle -----------------------------------------------------
    void do_check(const std::string& session, const std::string& id);
    void do_start(const std::string& session, const std::string& id);

    /// The session a report was found under, or empty if no longer tracked.
    std::string report_session(const std::string& id) const;
    void do_complete(const std::string& id);
    void do_abort(const std::string& id);
    void do_fail(const std::string& id, const std::string& error);

    void execute_action(const std::string& id, std::string_view action,
                        PgQuery::ResultHandler on_result);
    void delete_report(const std::string& id);
    bool in_progress(const std::string& id) const;

    void on_fatal(const std::string& error);

    bool stopping_{false};

#ifdef REPORTSERVER_NOTIFY
    // -- Path single ----------------------------------------------------------
    void probe_notify();
    void read_mode();
    void start_notify();
    void on_row(const db_platform::NotifyRow& row);
    void start_waiting();
    void check_single(const db_platform::NotifyRow& row, const Pending& pending);
    void build_single(const std::string& id);
    void after_build_error(const std::string& id, const db_platform::NotifyRow& row, const std::string& error);
    void finish(const std::string& id, const db_platform::NotifyRow& row, std::string sql);
    void finish_direct(const std::string& id, const db_platform::NotifyRow& row, std::string sql);
    void give_back(const std::string& id, std::string_view reason);
    void forget(const std::string& id, std::int64_t token);
    bool single(const std::string& id) const;
    std::string body_limits() const;
    std::string action_sql(const std::string& id, std::string_view action) const;

    /// The report and the scope a row names. The payload's format waits for a
    /// decision (T1147: no session code in notify.queue); this is the one place
    /// that reads it.
    static std::optional<Pending> parse_payload(std::string_view payload);

    std::string node_;
    EventLoop*  loop_{nullptr};
    std::unique_ptr<db_platform::NotifySubscriber> notify_;
    bool        notify_possible_{false};  // the database has the notify module
    bool        probing_{false};
    bool        probed_{false};           // the probe answered: the poll knows its filter
    bool        reading_mode_{false};
    std::chrono::steady_clock::time_point next_probe_{};
    std::chrono::steady_clock::time_point next_mode_{};
    std::chrono::milliseconds lease_{0};   // of the channel report, read with its mode
    long        refused_lease_{-1};        // the lease last refused, said once

    // Rows taken while max_in_flight reports are out.
    struct Waiting
    {
        db_platform::NotifyRow row;
        Pending                pending;
        std::chrono::steady_clock::time_point taken;
    };
    std::deque<Waiting> waiting_;
#endif
};

} // namespace apostol

#endif // WITH_POSTGRESQL
